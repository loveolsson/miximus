#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/detail/dma_buf_export.hpp"
#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "logger/logger.hpp"
#include "nodes/cef/detail/media_input_renderer.hpp"
#include "nodes/cef/detail/runtime.hpp"
#include "nodes/cef/detail/task.hpp"
#include "wrapper/cef/media_input_abi.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace miximus;
using namespace std::chrono_literals;
namespace cef_detail = nodes::cef::detail;

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
    bool                            closed_{};
    bool                            red_{};
    bool                            green_{};
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    explicit client_s(gpu::device_s& gpu)
        : destination_(gpu.create_texture({640, 360}))
        , context_(gpu.create_recording_context(1))
        , compare_(destination_, MIXIMUS_CEF_COMPARE_SHADER)
        , counters_(gpu.create_buffer(8, gpu::host_access_e::read_write))
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
        std::cerr << "Page: " << message.ToString() << '\n';
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
            values->GetInt(1) == 0) {
            std::lock_guard lock(mutex_);
            token_ = values->GetString(0).ToString();
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
                compare_.record(*record,
                                destination_,
                                counters_,
                                green ? std::array<float, 4>{0, 1, 0, 1} : std::array<float, 4>{1, 0, 0, 1},
                                0.01F);
                if (record->submit().wait(5s) != gpu::wait_result_e::ready)
                    throw std::runtime_error("GPU comparison failed");
                record.reset();
                uint32_t   mismatches{};
                const auto bytes = counters_.readable_bytes();
                std::memcpy(&mismatches, bytes.data(), sizeof(mismatches));
                if (mismatches == 0) {
                    std::lock_guard lock(mutex_);
                    (green ? green_ : red_) = true;
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
        if (!changed_.wait_for(lock, 15s, [&] { return !error_.empty() || (browser_ && !token_.empty()); }))
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
            throw std::runtime_error("Browser did not paint both GPU input colors");
        if (!error_.empty())
            throw std::runtime_error(error_);
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

bool send(cef_wrapper::send_media_frame_t      api,
          const CefRefPtr<client_s>&           client,
          const gpu::detail::dma_buf_export_s& exported,
          int64_t                              timestamp)
{
    const auto                 descriptor = exported.descriptor();
    cef_wrapper::media_frame_s frame{.input            = 0,
                                     .fd               = descriptor.fd,
                                     .width            = descriptor.extent.width,
                                     .height           = descriptor.extent.height,
                                     .stride           = static_cast<uint32_t>(descriptor.stride),
                                     .offset           = descriptor.offset,
                                     .modifier         = descriptor.modifier,
                                     .allocation_bytes = exported.allocation_bytes(),
                                     .timestamp_us     = timestamp};
    auto                       result = std::make_shared<std::promise<std::pair<int, int>>>();
    auto                       future = result->get_future();
    auto [browser, token]             = client->endpoint();
    if (!CefPostTask(TID_UI, new cef_detail::task_s([api, browser, token, frame, result] {
                         auto*      pending = new std::shared_ptr<std::promise<std::pair<int, int>>>(result);
                         const auto done    = [](void* pointer, int safe, int delivered) {
                             std::unique_ptr<std::shared_ptr<std::promise<std::pair<int, int>>>> state(
                                 static_cast<std::shared_ptr<std::promise<std::pair<int, int>>>*>(pointer));
                             (*state)->set_value({safe, delivered});
                         };
                         if (!api(browser->GetIdentifier(), token.c_str(), &frame, done, pending)) {
                             delete pending;
                             result->set_value({1, 0});
                         }
                     })))
        throw std::runtime_error("Cannot post media input to CEF UI");
    if (future.wait_for(10s) != std::future_status::ready)
        throw std::runtime_error(
            "Input GPU retirement not established; export must remain quarantined until runtime shutdown");
    const auto [safe, delivered] = future.get();
    if (!safe)
        throw std::runtime_error("Input GPU retirement failed; export quarantined until runtime shutdown");
    return delivered != 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "Usage: cef_media_input_probe RUNTIME_DIRECTORY PROFILE_DIRECTORY\n";
        return 2;
    }
    std::cout.setf(std::ios::unitbuf);
    try {
        logger::init_loggers(spdlog::level::warn);
        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        options.external_image_import = true;
        gpu::device_s                 gpu(options);
        gpu::detail::dma_buf_export_s exported(gpu, {640, 360});
        auto                          source  = gpu.create_texture({640, 360});
        auto                          context = gpu.create_recording_context(1);
        // Runtime shuts down before exporter destruction, including failure paths.
        cef_detail::runtime_s runtime(argv[1], argv[2]);
        const auto            api =
            reinterpret_cast<cef_wrapper::send_media_frame_t>(dlsym(RTLD_DEFAULT, cef_wrapper::SEND_MEDIA_FRAME));
        if (!api)
            throw std::runtime_error("Runtime does not provide the experimental media-input bridge");
        CefRefPtr<client_s> client = new client_s(gpu);
        const std::string   page =
            R"HTML(<!doctype html><style>html,body{margin:0;width:100%;height:100%;background:black}video{width:100%;height:100%;object-fit:fill}</style><video muted autoplay playsinline></video><script>miximus.getInputMediaStream({inputIndex:0}).then(s=>{const v=document.querySelector('video');v.srcObject=s;return v.play()}).catch(e=>console.error(String(e)))</script>)HTML";
        if (!CefPostTask(TID_UI, new cef_detail::task_s([client, page] {
                             CefWindowInfo window;
                             window.SetAsWindowless(0);
                             window.shared_texture_enabled = true;
                             CefBrowserSettings settings;
                             settings.windowless_frame_rate = 60;
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
            gpu::draw_s conversion;
            conversion.compositing         = gpu::compositing_e::replace;
            int                  delivered = 0;
            std::vector<int64_t> hold_us;
            for (int frame = 0; frame < 120; ++frame) {
                auto record = context.try_record();
                if (!record)
                    throw std::runtime_error("Export recording unavailable");
                record->clear(source, frame < 60 ? std::array<float, 4>{1, 0, 0, 1} : std::array<float, 4>{0, 1, 0, 1});
                exported.copy(*record, source, conversion);
                if (record->submit().wait(5s) != gpu::wait_result_e::ready)
                    throw std::runtime_error("Producer GPU completion failed");
                record.reset();
                const auto started = std::chrono::steady_clock::now();
                delivered += send(api, client, exported, int64_t(frame) * 16667);
                hold_us.push_back(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                        .count());
                std::this_thread::sleep_for(16ms);
            }
            std::ranges::sort(hold_us);
            std::cout << "Delivered " << delivered << "/120; send-to-reuse us p50=" << hold_us[59]
                      << " p95=" << hold_us[113] << " max=" << hold_us.back() << '\n';
            client->wait_colors();
        } catch (...) {
            client->close();
            throw;
        }
        client->close();
        if (gpu.validation_errors())
            throw std::runtime_error("Vulkan validation errors");
        std::cout << "Native input track painted red and green through GPU-only ingress and accelerated output\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
