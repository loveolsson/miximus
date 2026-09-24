#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/detail/dma_buf_export.hpp"
#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "logger/logger.hpp"
#include "nodes/cef/detail/media_input_exports.hpp"
#include "nodes/cef/detail/media_input_renderer.hpp"
#include "nodes/cef/detail/runtime.hpp"
#include "nodes/cef/detail/task.hpp"
#include "wrapper/cef/media_input_abi.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {
using namespace miximus;
using namespace std::chrono_literals;
namespace cef_detail = nodes::cef::detail;

std::array<float, 4> input_color(uint32_t input, bool second_phase)
{
    const uint32_t bits  = ((input + 1) & 7) ^ (second_phase ? 3 : 0);
    const float    alpha = input == 7 && second_phase ? 0.0F : 0.5F + float(input) / 16.0F;
    return {
        (bits & 1 ? 0.6F : 0.08F) * alpha, (bits & 2 ? 0.6F : 0.08F) * alpha, (bits & 4 ? 0.6F : 0.08F) * alpha, alpha};
}

class client_s final
    : public CefClient
    , public CefRenderHandler
    , public CefLifeSpanHandler
    , public CefDisplayHandler
{
    gpu::texture_s                  destination_;
    gpu::recording_context_s        context_;
    gpu::detail::color_comparison_s compare_;
    gpu::buffer_s                   counters_;
    std::mutex                      mutex_;
    std::condition_variable         changed_;
    CefRefPtr<CefBrowser>           browser_;
    std::string                     token_;
    std::string                     error_;
    const uint32_t                  input_count_;
    uint32_t                        subscribed_{};
    bool                            stats_ready_{};
    bool                            closed_{};
    bool                            red_{};
    bool                            green_{};
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    explicit client_s(gpu::device_s& gpu, uint32_t inputs)
        : destination_(gpu.create_texture({640, 360}))
        , context_(gpu.create_recording_context(1))
        , compare_(destination_, MIXIMUS_CEF_COMPARE_SHADER)
        , counters_(gpu.create_buffer(8, gpu::host_access_e::read_write))
        , input_count_(inputs)
    {
    }
    CefRefPtr<CefRenderHandler>   GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override { return this; }
    void GetViewRect(CefRefPtr<CefBrowser> /* browser */, CefRect& rect) override { rect = {0, 0, 640, 360}; }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        std::lock_guard lock(mutex_);
        browser_ = browser;
        changed_.notify_all();
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> /* browser */) override
    {
        std::lock_guard lock(mutex_);
        browser_ = nullptr;
        closed_  = true;
        changed_.notify_all();
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser> /* browser */,
                          cef_log_severity_t /* level */,
                          const CefString& message,
                          const CefString& /* source */,
                          int /* line */) override
    {
        const auto text = message.ToString();
        if (text.starts_with("probe-stats:")) {
            std::lock_guard lock(mutex_);
            stats_ready_ = true;
            changed_.notify_all();
        }
        std::cerr << "Page: " << text << '\n';
        return false;
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /* browser */,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        if (source != PID_RENDERER || !frame->IsMain() || message->GetName() != cef_detail::MEDIA_INPUT_SUBSCRIBE)
            return false;
        const auto values = message->GetArgumentList();
        if (values->GetSize() == 2 && values->GetType(0) == VTYPE_STRING && values->GetType(1) == VTYPE_INT &&
            values->GetInt(1) >= 0 && static_cast<uint32_t>(values->GetInt(1)) < input_count_) {
            std::lock_guard lock(mutex_);
            token_ = values->GetString(0).ToString();
            subscribed_ |= 1u << values->GetInt(1);
            changed_.notify_all();
        }
        return true;
    }
    void creation_failed()
    {
        std::lock_guard lock(mutex_);
        error_  = "Browser creation rejected";
        closed_ = true;
        changed_.notify_all();
    }
    void fail(std::string error)
    {
        std::lock_guard lock(mutex_);
        error_ = std::move(error);
        changed_.notify_all();
    }
    void OnPaint(CefRefPtr<CefBrowser> /* browser */,
                 PaintElementType /* type */,
                 const RectList& /* rects */,
                 const void* /* pixels */,
                 int /* width */,
                 int /* height */) override
    {
        fail("Software browser output; no CPU pixels consumed");
    }
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> /* browser */,
                            PaintElementType type,
                            const RectList& /* rects */,
                            const CefAcceleratedPaintInfo& info) override
    {
        if (type != PET_VIEW)
            return;
        try {
            if (info.plane_count != 1 ||
                (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888))
                throw std::runtime_error("Unexpected browser output layout");
            gpu::detail::dma_buf_image_s descriptor{
                .fd     = info.planes[0].fd,
                .extent = {static_cast<uint32_t>(info.extra.coded_size.width),
                           static_cast<uint32_t>(info.extra.coded_size.height)},
                .order =
                    info.format == CEF_COLOR_TYPE_BGRA_8888 ? gpu::channel_order_e::bgra : gpu::channel_order_e::rgba,
                .modifier = info.modifier,
                .offset   = info.planes[0].offset,
                .stride   = info.planes[0].stride
            };
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
            auto record            = context_.try_record();
            if (!record ||
                gpu::detail::dma_buf_copy_s::submit(*record, descriptor, destination_, conversion, 500ms).wait(5s) !=
                    gpu::wait_result_e::ready)
                throw std::runtime_error("Browser output GPU copy failed");
            record.reset();
            for (bool green : {false, true}) {
                std::ranges::fill(counters_.writable_bytes(), std::byte{});
                record = context_.try_record();
                if (!record)
                    throw std::runtime_error("Comparison recording exhausted");
                for (uint32_t input = 0; input < input_count_; ++input)
                    compare_.record(*record,
                                    destination_,
                                    counters_,
                                    input_color(input, green),
                                    0.01F,
                                    640 * input / input_count_,
                                    640 * (input + 1) / input_count_);
                if (record->submit().wait(5s) != gpu::wait_result_e::ready)
                    throw std::runtime_error("GPU comparison failed");
                record.reset();
                uint32_t   mismatches{};
                const auto bytes = counters_.readable_bytes();
                std::memcpy(&mismatches, bytes.data(), sizeof(mismatches));
                if (mismatches == 0) {
                    std::lock_guard lock(mutex_);
                    if (!green)
                        red_ = true;
                    else if (red_)
                        green_ = true;
                    changed_.notify_all();
                }
            }
        } catch (const std::exception& error) {
            fail(error.what());
        }
    }
    void wait_ready()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 15s, [&] {
                return !error_.empty() || (browser_ && !token_.empty() && subscribed_ == ((1u << input_count_) - 1));
            }))
            throw std::runtime_error("Page did not subscribe to a native input track");
        if (!error_.empty())
            throw std::runtime_error(error_);
    }
    std::pair<CefRefPtr<CefBrowser>, std::string> endpoint()
    {
        std::lock_guard lock(mutex_);
        return {browser_, token_};
    }
    void wait_colors()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 10s, [&] { return !error_.empty() || (red_ && green_); }))
            throw std::runtime_error("Browser did not paint both ordered per-input GPU color patterns");
        if (!error_.empty())
            throw std::runtime_error(error_);
    }
    void report_video_frames()
    {
        auto [browser, token] = endpoint();
        if (!CefPostTask(TID_UI, new cef_detail::task_s([browser] {
                             browser->GetMainFrame()->ExecuteJavaScript(
                                 "console.log('probe-stats:'+JSON.stringify(globalThis.probeStats))", "probe-stats", 1);
                         })))
            throw std::runtime_error("Cannot collect video frame metadata");
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 2s, [&] { return stats_ready_; }))
            throw std::runtime_error("Video frame metadata timed out");
    }
    void close()
    {
        auto [browser, token] = endpoint();
        if (browser)
            CefPostTask(TID_UI, new cef_detail::task_s([browser] { browser->GetHost()->CloseBrowser(true); }));
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 10s, [&] { return closed_; }))
            std::terminate();
    }
};

cef_wrapper::media_frame_s describe(const gpu::detail::dma_buf_export_s& exported, uint32_t input, int64_t timestamp)
{
    const auto d = exported.descriptor();
    return {.input            = input,
            .fd               = d.fd,
            .width            = d.extent.width,
            .height           = d.extent.height,
            .stride           = static_cast<uint32_t>(d.stride),
            .offset           = d.offset,
            .modifier         = d.modifier,
            .allocation_bytes = exported.allocation_bytes(),
            .timestamp_us     = timestamp};
}

std::future<std::pair<int, int>> enqueue(cef_wrapper::send_media_frame_t api,
                                         const CefRefPtr<client_s>&      client,
                                         cef_wrapper::media_frame_s      frame,
                                         std::function<void(bool)>       retire = {})
{
    auto result           = std::make_shared<std::promise<std::pair<int, int>>>();
    auto future           = result->get_future();
    auto [browser, token] = client->endpoint();
    if (!CefPostTask(TID_UI, new cef_detail::task_s([api, browser, token, frame, result, retire] {
                         struct pending_s
                         {
                             std::shared_ptr<std::promise<std::pair<int, int>>> result;
                             std::function<void(bool)>                          retire;
                         };
                         auto*      pending = new pending_s{result, retire};
                         const auto done    = [](void* pointer, int safe, int delivered) {
                             std::unique_ptr<pending_s> state(static_cast<pending_s*>(pointer));
                             if (state->retire)
                                 state->retire(safe != 0);
                             state->result->set_value({safe, delivered});
                         };
                         if (!browser || !api(browser->GetIdentifier(), token.c_str(), &frame, done, pending))
                             done(pending, 1, 0);
                     }))) {
        if (retire)
            retire(true); // No IPC/native access was started.
        throw std::runtime_error("Cannot post media input to CEF UI");
    }
    return future;
}

bool send(cef_wrapper::send_media_frame_t      api,
          const CefRefPtr<client_s>&           client,
          const gpu::detail::dma_buf_export_s& exported,
          int64_t                              timestamp,
          uint32_t                             input)
{
    auto future = enqueue(api, client, describe(exported, input, timestamp));
    if (future.wait_for(10s) != std::future_status::ready)
        throw std::runtime_error("Input GPU retirement not established; export quarantined until runtime shutdown");
    const auto [safe, delivered] = future.get();
    if (!safe)
        throw std::runtime_error("Input GPU retirement failed; export quarantined until runtime shutdown");
    return delivered != 0;
}

void run_async(cef_detail::media_input_exports_s& queue,
               gpu::texture_s&                    source,
               gpu::recording_context_s&          context,
               cef_wrapper::send_media_frame_t    api,
               const CefRefPtr<client_s>&         client,
               uint32_t                           inputs)
{
    struct pending_s
    {
        std::future<std::pair<int, int>>      result;
        std::chrono::steady_clock::time_point started;
    };
    std::exception_ptr   error;
    uint64_t             delivered{};
    std::vector<int64_t> hold_us;
    std::jthread         transfer([&](std::stop_token stop) {
        try {
            std::vector<pending_s>                               pending;
            std::optional<std::chrono::steady_clock::time_point> drain_started;
            for (;;) {
                if (queue.failed())
                    throw std::runtime_error("Export queue quarantined an unproven GPU read");
                for (auto it = pending.begin(); it != pending.end();) {
                    if (it->result.wait_for(0ms) != std::future_status::ready) {
                        ++it;
                        continue;
                    }
                    const auto [safe, accepted] = it->result.get();
                    if (!safe)
                        throw std::runtime_error("Chromium did not establish input GPU retirement");
                    delivered += accepted != 0;
                    hold_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - it->started)
                                          .count());
                    it = pending.erase(it);
                }
                while (auto frame = queue.poll()) {
                    const auto started = std::chrono::steady_clock::now();
                    auto       description =
                        describe(frame->image(), static_cast<uint32_t>(frame->ticket().input), frame->timestamp_us());
                    description.source_generation = frame->ticket().generation;
                    pending.push_back(
                        {enqueue(api, client, description, [frame](bool safe) { frame->retire(safe); }), started});
                }
                if (stop.stop_requested()) {
                    if (!drain_started)
                        drain_started = std::chrono::steady_clock::now();
                    if (queue.idle() && pending.empty())
                        break;
                    if (std::chrono::steady_clock::now() - *drain_started > 10s)
                        throw std::runtime_error("Asynchronous input drain timed out");
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch (...) {
            error = std::current_exception();
        }
    });
    const auto           start = std::chrono::steady_clock::now();
    try {
        for (int frame = 0; frame < 120; ++frame) {
            if (auto commands = context.try_record()) {
                for (uint32_t input = 0; input < inputs; ++input) {
                    commands->clear(source, input_color(input, frame >= 60));
                    if (auto publication = queue.record(input, *commands, source, int64_t(frame) * 16667))
                        commands->on_submitted([publication](gpu::completion_s) { publication->commit(); });
                }
                (void)commands->submit();
            }
            std::this_thread::sleep_until(start + (frame + 1) * 16667us);
        }
    } catch (...) {
        transfer.request_stop();
        transfer.join();
        throw;
    }
    transfer.request_stop();
    transfer.join();
    if (error)
        std::rethrow_exception(error);
    uint64_t admitted{}, drops{};
    for (uint32_t input = 0; input < inputs; ++input) {
        admitted += queue.metrics(input).admitted;
        drops += queue.metrics(input).capacity_drops;
    }
    std::ranges::sort(hold_us);
    std::cout << "Async 60 Hz: admitted=" << admitted << " delivered=" << delivered << " capacity_drops=" << drops;
    if (!hold_us.empty())
        std::cout << "; send-to-reuse us p50=" << hold_us[(hold_us.size() - 1) / 2]
                  << " p95=" << hold_us[(hold_us.size() - 1) * 95 / 100] << " max=" << hold_us.back();
    std::cout << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: cef_media_input_probe RUNTIME_DIRECTORY PROFILE_DIRECTORY [INPUT_COUNT=1] "
                     "[ASYNC_EXPORT_DEPTH=1..8]\n";
        return 2;
    }
    std::cout.setf(std::ios::unitbuf);
    try {
        uint32_t inputs = 1;
        if (argc >= 4) {
            const std::string_view value(argv[3]);
            const auto             parsed = std::from_chars(value.data(), value.data() + value.size(), inputs);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || inputs < 1 || inputs > 8)
                throw std::runtime_error("INPUT_COUNT must be 1 through 8");
        }
        uint32_t async_depth{};
        if (argc == 5) {
            const std::string_view value(argv[4]);
            const auto             parsed = std::from_chars(value.data(), value.data() + value.size(), async_depth);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || async_depth < 1 ||
                async_depth > 8)
                throw std::runtime_error("ASYNC_EXPORT_DEPTH must be 1 through 8");
        }
        logger::init_loggers(spdlog::level::warn);
        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        options.external_image_import = true;
        gpu::device_s                                               gpu(options);
        std::vector<std::unique_ptr<gpu::detail::dma_buf_export_s>> exports;
        for (uint32_t input = 0; !async_depth && input < inputs; ++input)
            exports.push_back(std::make_unique<gpu::detail::dma_buf_export_s>(gpu, gpu::extent_s{640, 360}));
        auto                                             source  = gpu.create_texture({640, 360});
        auto                                             context = gpu.create_recording_context(3);
        cef_detail::media_input_exports_s::quarantine_s  quarantine;
        std::optional<cef_detail::media_input_exports_s> export_queue;
        if (async_depth) {
            export_queue.emplace(gpu, async_depth, quarantine);
            for (uint32_t input = 0; input < inputs; ++input)
                if (!export_queue->configure(input, {640, 360}))
                    throw std::runtime_error("Cannot configure export queue");
        }
        // Runtime shuts down before exporter destruction, including failure paths.
        cef_detail::runtime_s runtime(argv[1], argv[2]);
        const auto            api =
            reinterpret_cast<cef_wrapper::send_media_frame_t>(dlsym(RTLD_DEFAULT, cef_wrapper::SEND_MEDIA_FRAME));
        if (!api)
            throw std::runtime_error("Runtime does not provide the experimental media-input bridge");
        CefRefPtr<client_s> client = new client_s(gpu, inputs);
        std::string         page =
            R"HTML(<!doctype html><style>html,body{margin:0;width:100%;height:100%;background:transparent;overflow:hidden}body{display:flex}video{display:block;min-width:0;flex:1;width:0;height:100%;object-fit:fill}</style>)HTML";
        for (uint32_t input = 0; input < inputs; ++input)
            page += std::format("<video style=\"flex:none;width:{}px\" muted autoplay playsinline></video>",
                                640 * (input + 1) / inputs - 640 * input / inputs);
        page +=
            R"HTML(<script>globalThis.probeStats=[];document.querySelectorAll('video').forEach((v,inputIndex)=>miximus.getInputMediaStream({inputIndex}).then(async s=>{if(await miximus.getInputMediaStream({inputIndex})!==s)throw Error('Stream identity changed');const t=s.getVideoTracks()[0],clone=t.clone();t.stop();const next=await miximus.getInputMediaStream({inputIndex});if(next.getVideoTracks()[0].readyState!=='live'||clone.readyState!=='live')throw Error('Reacquisition stopped a live track');clone.stop();const stats={callbacks:0,presented:0,lastMediaTime:0};probeStats[inputIndex]=stats;const observe=(now,m)=>{stats.callbacks++;stats.presented=m.presentedFrames;stats.lastMediaTime=m.mediaTime;v.requestVideoFrameCallback(observe)};v.requestVideoFrameCallback(observe);v.srcObject=next;return v.play()}).catch(e=>console.error(String(e))))</script>)HTML";
        if (!CefPostTask(TID_UI, new cef_detail::task_s([client, page] {
                             CefWindowInfo window;
                             window.SetAsWindowless(0);
                             window.shared_texture_enabled = true;
                             CefBrowserSettings settings;
                             settings.windowless_frame_rate = 60;
                             settings.background_color      = CefColorSetARGB(0, 0, 0, 0);
                             if (!CefBrowserHost::CreateBrowser(window,
                                                                client,
                                                                "data:text/html," +
                                                                    CefURIEncode(page, false).ToString(),
                                                                settings,
                                                                nullptr,
                                                                nullptr))
                                 client->creation_failed();
                         })))
            throw std::runtime_error("Cannot create probe browser");
        try {
            client->wait_ready();
            if (export_queue) {
                run_async(*export_queue, source, context, api, client, inputs);
            } else {
                gpu::draw_s conversion;
                conversion.compositing         = gpu::compositing_e::replace;
                conversion.transfer            = gpu::color_operation_e::encode_srgb_premultiplied;
                int                  delivered = 0;
                std::vector<int64_t> hold_us;
                for (int frame = 0; frame < 120; ++frame) {
                    auto record = context.try_record();
                    if (!record)
                        throw std::runtime_error("Export recording unavailable");
                    for (uint32_t input = 0; input < inputs; ++input) {
                        record->clear(source, input_color(input, frame >= 60));
                        exports[input]->copy(*record, source, conversion);
                    }
                    if (record->submit().wait(5s) != gpu::wait_result_e::ready)
                        throw std::runtime_error("Producer GPU completion failed");
                    record.reset();
                    for (uint32_t input = 0; input < inputs; ++input) {
                        const auto started = std::chrono::steady_clock::now();
                        delivered += send(api, client, *exports[input], int64_t(frame) * 16667, input);
                        hold_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count());
                    }
                    std::this_thread::sleep_for(16ms);
                }
                std::ranges::sort(hold_us);
                std::cout << "Delivered " << delivered << "/" << 120 * inputs << " across " << inputs
                          << " inputs; send-to-reuse us p50=" << hold_us[hold_us.size() / 2 - 1]
                          << " p95=" << hold_us[hold_us.size() * 95 / 100 - 1] << " max=" << hold_us.back() << '\n';
            }
            client->wait_colors();
            client->report_video_frames();
            if (!export_queue) {
                // Metadata invalidation must reject a subsequently arriving old
                // frame without importing it or manufacturing a GPU completion.
                auto await = [](std::future<std::pair<int, int>> result) {
                    if (result.wait_for(5s) != std::future_status::ready)
                        throw std::runtime_error("Generation qualification timed out");
                    const auto response = result.get();
                    if (!response.first)
                        throw std::runtime_error("Generation qualification did not establish safe retirement");
                    return response.second;
                };
                if (await(enqueue(api, client, {.source_generation = 2, .invalidate_only = 1})))
                    throw std::runtime_error("Metadata invalidation delivered a video frame");
                auto packet = describe(*exports[0], 0, 2'100'000);
                if (await(enqueue(api, client, packet)))
                    throw std::runtime_error("Superseded source generation entered the media source");
                packet.source_generation = 2;
                if (!await(enqueue(api, client, packet)))
                    throw std::runtime_error("Current source generation was not admitted");
                std::cout << "Metadata invalidation rejected the old generation and admitted its replacement\n";
            }
        } catch (...) {
            client->close();
            throw;
        }
        client->close();
        if (gpu.validation_errors())
            throw std::runtime_error("Vulkan validation errors");
        std::cout << "Native input tracks painted both ordered per-input color patterns through GPU-only ingress and "
                     "accelerated output\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
