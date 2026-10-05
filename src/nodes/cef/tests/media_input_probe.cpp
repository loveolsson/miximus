#include "gpu/detail/external_image_copy.hpp"
#include "gpu/detail/external_image_export.hpp"
#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "logger/logger.hpp"
#include "nodes/cef/detail/image_transport.hpp"
#include "nodes/cef/detail/media_input_exports.hpp"
#include "nodes/cef/detail/media_input_renderer.hpp"
#include "nodes/cef/detail/runtime.hpp"
#include "nodes/cef/detail/task.hpp"
#include "wrapper/cef/media_input_abi.hpp"
#include "wrapper/cef/platform.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace miximus;
using namespace std::chrono_literals;
namespace cef_detail = nodes::cef::detail;

std::array<float, 4> input_color(uint32_t input, bool second_phase)
{
    const uint32_t bits  = ((input + 1) & 7) ^ (second_phase ? 3 : 0);
    const float    alpha = input == 7 && second_phase ? 0.0F : 0.5F + (float(input) / 16.0F);
    return {(((bits & 1) != 0U) ? 0.6F : 0.08F) * alpha,
            (((bits & 2) != 0U) ? 0.6F : 0.08F) * alpha,
            (((bits & 4) != 0U) ? 0.6F : 0.08F) * alpha,
            alpha};
}

struct expected_region_s
{
    uint32_t             begin;
    uint32_t             end;
    std::array<float, 4> color;
};

class client_s final
    : public CefClient
    , public CefRenderHandler
    , public CefLifeSpanHandler
    , public CefDisplayHandler
{
    gpu::texture_s                      destination_;
    gpu::recording_context_s            context_;
    gpu::detail::color_comparison_s     compare_;
    gpu::buffer_s                       counters_;
    std::mutex                          mutex_;
    std::condition_variable             changed_;
    CefRefPtr<CefBrowser>               browser_;
    std::string                         token_;
    std::string                         error_;
    std::string                         input_error_;
    const uint32_t                      input_count_;
    uint32_t                            subscribed_{};
    bool                                stats_ready_{};
    bool                                closed_{};
    bool                                red_{};
    bool                                green_{};
    std::optional<std::array<float, 4>> expected_;
    uint64_t                            expectation_{};
    bool                                expectation_matched_{};
    std::vector<expected_region_s>      regions_;
    uint32_t                            last_mismatches_{};
    float                               last_maximum_error_{};
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    explicit client_s(gpu::device_s& gpu, uint32_t inputs)
        : destination_(gpu.create_texture({.width = 640, .height = 360}))
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
        std::scoped_lock lock(mutex_);
        browser_ = browser;
        changed_.notify_all();
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> /* browser */) override
    {
        std::scoped_lock lock(mutex_);
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
            std::scoped_lock lock(mutex_);
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
        if (source != PID_RENDERER || !frame->IsMain()) {
            return false;
        }

        const auto values = message->GetArgumentList();
        if (message->GetName() == cef_detail::MEDIA_INPUT_FAILURE) {
            std::scoped_lock lock(mutex_);
            if (values->GetSize() == 2 && values->GetType(0) == VTYPE_STRING &&
                values->GetString(0).ToString() == token_ && values->GetType(1) == VTYPE_STRING) {
                input_error_ = values->GetString(1).ToString();
                changed_.notify_all();
            }

            return true;
        }

        if (message->GetName() != cef_detail::MEDIA_INPUT_ACTIVITY) {
            return false;
        }

        if (values->GetSize() == 4 && values->GetType(0) == VTYPE_STRING && values->GetType(1) == VTYPE_INT &&
            values->GetType(2) == VTYPE_BOOL && values->GetInt(1) >= 0 &&
            std::cmp_less(values->GetInt(1), input_count_)) {
            std::scoped_lock lock(mutex_);
            token_ = values->GetString(0).ToString();
            if (values->GetBool(2)) {
                subscribed_ |= 1U << values->GetInt(1);
            } else {
                subscribed_ &= ~(1U << values->GetInt(1));
            }

            changed_.notify_all();
        }

        return true;
    }

    void wait_input_failure()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return !input_error_.empty(); })) {
            throw std::runtime_error("Failed bridge did not report its document-scoped error");
        }

        std::cout << "Renderer reported input failure: " << input_error_ << '\n';
    }

    void creation_failed()
    {
        std::scoped_lock lock(mutex_);
        error_  = "Browser creation rejected";
        closed_ = true;
        changed_.notify_all();
    }

    void fail(std::string error)
    {
        std::scoped_lock lock(mutex_);
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

    void
    accept_comparison(uint64_t expectation, bool has_expected, bool green, uint32_t mismatches, float maximum_error)
    {
        const std::scoped_lock lock(mutex_);
        if (expectation == expectation_) {
            last_mismatches_    = mismatches;
            last_maximum_error_ = maximum_error;
        }
        if (mismatches != 0) {
            return;
        }
        if (has_expected && expectation == expectation_) {
            expectation_matched_ = true;
        }
        if (!green) {
            red_ = true;
        } else if (red_) {
            green_ = true;
        }
        changed_.notify_all();
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser> /* browser */,
                            PaintElementType type,
                            const RectList& /* rects */,
                            const CefAcceleratedPaintInfo& info) override
    {
        if (type != PET_VIEW) {
            return;
        }

        try {
            const auto descriptor = nodes::cef::detail::capture_image(info);

            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
            auto record            = context_.try_record();
            if (!record ||
                gpu::detail::external_image_copy_s::submit(*record, descriptor, destination_, conversion, 500ms)
                        .wait(5s) != gpu::wait_result_e::ready) {
                throw std::runtime_error("Browser output GPU copy failed");
            }

            record.reset();
            std::optional<std::array<float, 4>> expected;
            uint64_t                            expectation{};
            std::vector<expected_region_s>      regions;
            {
                std::scoped_lock lock(mutex_);
                expected    = expected_;
                expectation = expectation_;
                regions     = regions_;
            }
            for (bool green : {false, true}) {
                std::ranges::fill(counters_.writable_bytes(), std::byte{});
                record = context_.try_record();
                if (!record) {
                    throw std::runtime_error("Comparison recording exhausted");
                }

                if (!regions.empty()) {
                    for (const auto& region : regions) {
                        compare_.record(
                            *record, destination_, counters_, region.color, 0.02F, region.begin, region.end);
                    }
                } else {
                    for (uint32_t input = 0; input < input_count_; ++input) {
                        compare_.record(*record,
                                        destination_,
                                        counters_,
                                        expected.value_or(input_color(input, green)),
                                        0.01F,
                                        640 * input / input_count_,
                                        640 * (input + 1) / input_count_);
                    }
                }

                if (record->submit().wait(5s) != gpu::wait_result_e::ready) {
                    throw std::runtime_error("GPU comparison failed");
                }

                record.reset();
                uint32_t   mismatches{};
                const auto bytes = counters_.readable_bytes();
                std::memcpy(&mismatches, bytes.data(), sizeof(mismatches));
                float maximum_error{};
                std::memcpy(&maximum_error, bytes.data() + sizeof(uint32_t), sizeof(float));
                accept_comparison(
                    expectation, expected.has_value() || !regions.empty(), green, mismatches, maximum_error);
            }
        } catch (const std::exception& error) {
            fail(error.what());
        }
    }

    void wait_ready()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 15s, [&] {
                return !error_.empty() || (browser_ && !token_.empty() && subscribed_ == ((1U << input_count_) - 1));
            })) {
            throw std::runtime_error("Page did not subscribe to a native input track");
        }

        if (!error_.empty()) {
            throw std::runtime_error(error_);
        }
    }

    std::pair<CefRefPtr<CefBrowser>, std::string> endpoint()
    {
        std::scoped_lock lock(mutex_);
        return {browser_, token_};
    }

    void expect(std::array<float, 4> color)
    {
        std::scoped_lock lock(mutex_);
        expected_ = color;
        ++expectation_;
        expectation_matched_ = false;
    }

    void expect_regions(std::vector<expected_region_s> regions)
    {
        std::scoped_lock lock(mutex_);
        regions_ = std::move(regions);
        ++expectation_;
        expectation_matched_ = false;
    }

    void wait_expected()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 10s, [&] { return !error_.empty() || expectation_matched_; })) {
            throw std::runtime_error(
                std::format("Output comparison failed: {} mismatched pixels, maximum linear error {}",
                            last_mismatches_,
                            last_maximum_error_));
        }
        if (!error_.empty()) {
            throw std::runtime_error(error_);
        }
        std::cout << "Output comparison: " << last_mismatches_ << " mismatches, maximum linear error "
                  << last_maximum_error_ << '\n';
    }

    void wait_colors()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 10s, [&] { return !error_.empty() || (red_ && green_); })) {
            throw std::runtime_error("Browser did not paint both ordered per-input GPU color patterns");
        }

        if (!error_.empty()) {
            throw std::runtime_error(error_);
        }
    }

    void report_video_frames()
    {
        auto [browser, token] = endpoint();
        if (!CefPostTask(TID_UI, new cef_detail::task_s([browser] {
                             browser->GetMainFrame()->ExecuteJavaScript(
                                 "console.log('probe-stats:'+JSON.stringify(globalThis.probeStats))", "probe-stats", 1);
                         }))) {
            throw std::runtime_error("Cannot collect video frame metadata");
        }

        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 2s, [&] { return stats_ready_; })) {
            throw std::runtime_error("Video frame metadata timed out");
        }
    }

    void close()
    {
        auto [browser, token] = endpoint();
        if (browser) {
            CefPostTask(TID_UI, new cef_detail::task_s([browser] { browser->GetHost()->CloseBrowser(true); }));
        }

        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 10s, [&] { return closed_; })) {
            std::terminate();
        }
    }
};

cef_wrapper::media_frame_s
describe(const gpu::detail::external_image_export_s& exported, uint32_t input, int64_t timestamp)
{
    const auto d      = exported.descriptor();
    auto       packet = cef_wrapper::make_media_frame();
    packet.input      = input;
    nodes::cef::detail::set_media_frame_image(packet, d, exported.allocation_bytes());
    packet.timestamp_us = timestamp;
    return packet;
}

std::future<std::pair<int, int>> enqueue(cef_wrapper::send_media_frame_t  api,
                                         const CefRefPtr<client_s>&       client,
                                         cef_wrapper::media_frame_s       frame,
                                         const std::function<void(bool)>& retire  = {},
                                         bool                             mipmaps = false)
{
    auto result           = std::make_shared<std::promise<std::pair<int, int>>>();
    auto future           = result->get_future();
    auto [browser, token] = client->endpoint();
    if (!CefPostTask(TID_UI, new cef_detail::task_s([api, browser, token, frame, result, retire, mipmaps] {
                         struct pending_s
                         {
                             std::shared_ptr<std::promise<std::pair<int, int>>> result;
                             std::function<void(bool)>                          retire;
                         };

                         auto*      pending = new pending_s{.result = result, .retire = retire};
                         const auto done    = [](void* pointer, int safe, int delivered) {
                             std::unique_ptr<pending_s> state(static_cast<pending_s*>(pointer));
                             if (state->retire) {
                                 state->retire(safe != 0);
                             }

                             state->result->set_value({safe, delivered});
                         };

                         if (!browser ||
                             !api(browser->GetIdentifier(), token.c_str(), &frame, mipmaps, done, pending)) {
                             done(pending, 1, 0);
                         }
                     }))) {
        if (retire) {
            retire(true); // No IPC/native access was started.
        }

        throw std::runtime_error("Cannot post media input to CEF UI");
    }

    return future;
}

bool send(cef_wrapper::send_media_frame_t             api,
          const CefRefPtr<client_s>&                  client,
          const gpu::detail::external_image_export_s& exported,
          int64_t                                     timestamp,
          uint32_t                                    input,
          bool                                        mipmaps = false)
{
    auto future = enqueue(api, client, describe(exported, input, timestamp), {}, mipmaps);
    if (future.wait_for(10s) != std::future_status::ready) {
        throw std::runtime_error("Input GPU retirement not established; export quarantined until runtime shutdown");
    }

    const auto [safe, delivered] = future.get();
    if (safe == 0) {
        throw std::runtime_error("Input GPU retirement failed; export quarantined until runtime shutdown");
    }

    return delivered != 0;
}

// One bright column in every four is invisible to a center-aligned bilinear
// 4:1 reduction, but contributes exactly 1/4 to a mipmapped reduction. Change
// colors and toggle on the same track to exercise pooled destination reuse.
void run_mipmaps(gpu::device_s&                        gpu,
                 gpu::detail::external_image_export_s& exported,
                 gpu::texture_s&                       pattern,
                 gpu::recording_context_s&             context,
                 cef_wrapper::send_media_frame_t       api,
                 const CefRefPtr<client_s>&            client)
{
    auto    solid     = gpu.create_texture({.width = 1, .height = 1});
    int64_t timestamp = 0;
    for (int stage = 0; stage < 5; ++stage) {
        const bool mipmaps = stage != 0 && stage != 3;
        const bool green   = stage == 2;
        // Mips average the stored sRGB-encoded RGBA values. Output comparison
        // decodes sRGB, so 0.25 becomes approximately 0.050876 linear.
        const float average = mipmaps ? 0.050876F : 0.0F;
        client->expect({green ? 0.0F : average, green ? average : 0.0F, 0.0F, 1.0F});
        for (int frame = 0; frame < 12; ++frame) {
            auto record = context.try_record();
            if (!record) {
                throw std::runtime_error("Mipmap probe recording unavailable");
            }
            record->clear(pattern, {0, 0, 0, 1});
            record->clear(solid, green ? std::array<float, 4>{0, 1, 0, 1} : std::array<float, 4>{1, 0, 0, 1});
            for (int x = 0; x < 2560; x += 4) {
                record->draw(solid,
                             pattern,
                             {
                                 .destination = {float(x), 0, 1, 1440},
                                   .compositing = gpu::compositing_e::replace
                });
            }
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::encode_srgb_premultiplied;
            exported.copy(*record, pattern, conversion);
            if (record->submit().wait(5s) != gpu::wait_result_e::ready) {
                throw std::runtime_error("Mipmap pattern GPU completion failed");
            }
            record.reset();
            timestamp += 16667;
            (void)send(api, client, exported, timestamp, 0, mipmaps);
            std::this_thread::sleep_for(20ms);
        }
        client->wait_expected();
        std::cout << "Mipmap stage " << stage << " passed (enabled=" << mipmaps << ")\n";
    }
}

std::vector<expected_region_s> anisotropic_regions(bool enabled, bool invert)
{
    std::vector<expected_region_s> regions;
    for (uint32_t x = 64; x < 576; x += 16) {
        const float red = (((x / 4) & 1) != 0U) != invert ? 0.0F : 1.0F;
        regions.push_back({
            .begin = x + 1, .end = x + 3, .color = {red, enabled ? 0.050876F : 0.0F, 0, 1}
        });
        regions.push_back({
            .begin = x + 5, .end = x + 7, .color = {1.0F - red, enabled ? 0.050876F : 0.0F, 0, 1}
        });
    }
    return regions;
}

// An 8:1 vertical reduction with no horizontal reduction. Red bars must stay
// sharp in X (isotropic mips fail), while two green rows in eight must contribute
// in Y (bilinear fails). Compare stripe interiors, avoiding filter boundaries.
void run_anisotropy(gpu::device_s&                        gpu,
                    gpu::detail::external_image_export_s& exported,
                    gpu::texture_s&                       pattern,
                    gpu::recording_context_s&             context,
                    cef_wrapper::send_media_frame_t       api,
                    const CefRefPtr<client_s>&            client)
{
    auto    solid     = gpu.create_texture({.width = 1, .height = 1});
    int64_t timestamp = 0;
    for (int stage = 0; stage < 3; ++stage) {
        const bool enabled = stage != 0;
        const bool invert  = stage == 2;
        client->expect_regions(anisotropic_regions(enabled, invert));
        for (int frame = 0; frame < 12; ++frame) {
            auto record = context.try_record();
            if (!record) {
                throw std::runtime_error("Anisotropic probe recording unavailable");
            }
            record->clear(pattern, {0, 0, 0, 1});
            record->clear(solid, {1, 0, 0, 1});
            for (int x = invert ? 4 : 0; x < 640; x += 8) {
                record->draw(solid,
                             pattern,
                             {
                                 .destination = {float(x), 0, 4, 2880},
                                   .compositing = gpu::compositing_e::replace
                });
            }
            // Add green without changing the red test pattern.
            record->clear(solid, {0, 1, 0, 0});
            for (int y = 0; y < 2880; y += 8) {
                record->draw(solid,
                             pattern,
                             {
                                 .destination = {0, float(y), 640, 2},
                                   .compositing = gpu::compositing_e::source_over
                });
            }
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::encode_srgb_premultiplied;
            exported.copy(*record, pattern, conversion);
            if (record->submit().wait(5s) != gpu::wait_result_e::ready) {
                throw std::runtime_error("Anisotropic probe GPU completion failed");
            }
            record.reset();
            timestamp += 16667;
            (void)send(api, client, exported, timestamp, 0, enabled);
            std::this_thread::sleep_for(20ms);
        }
        client->wait_expected();
        std::cout << "Anisotropic output stage " << stage << " passed (enabled=" << enabled << ")\n";
    }
}

struct pending_s
{
    std::future<std::pair<int, int>>      result;
    std::chrono::steady_clock::time_point started;
};

struct transfer_result_s
{
    std::exception_ptr   error;
    uint64_t             delivered{};
    std::vector<int64_t> hold_us;
};

void retire_completed(std::vector<pending_s>& pending, transfer_result_s& result)
{
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->result.wait_for(0ms) != std::future_status::ready) {
            ++it;
            continue;
        }

        const auto [safe, accepted] = it->result.get();
        if (safe == 0) {
            throw std::runtime_error("Chromium did not establish input GPU retirement");
        }

        result.delivered += static_cast<uint64_t>(accepted != 0);
        result.hold_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - it->started)
                .count());
        it = pending.erase(it);
    }
}

void transfer_frames(const std::stop_token&             stop,
                     cef_detail::media_input_exports_s& queue,
                     cef_wrapper::send_media_frame_t    api,
                     const CefRefPtr<client_s>&         client,
                     transfer_result_s&                 result)
{
    try {
        std::vector<pending_s>                               pending;
        std::optional<std::chrono::steady_clock::time_point> drain_started;
        for (;;) {
            if (queue.failed()) {
                throw std::runtime_error("Export queue quarantined an unproven GPU read");
            }

            retire_completed(pending, result);

            while (auto frame = queue.poll()) {
                const auto started = std::chrono::steady_clock::now();
                auto       description =
                    describe(frame->image(), static_cast<uint32_t>(frame->ticket().input), frame->timestamp_us());
                description.source_generation = frame->ticket().generation;
                pending.push_back({
                    .result  = enqueue(api, client, description, [frame](bool safe) { frame->retire(safe); }),
                    .started = started,
                });
            }

            if (stop.stop_requested()) {
                if (!drain_started) {
                    drain_started = std::chrono::steady_clock::now();
                }

                if (queue.idle() && pending.empty()) {
                    break;
                }

                if (std::chrono::steady_clock::now() - *drain_started > 10s) {
                    throw std::runtime_error("Asynchronous input drain timed out");
                }
            }

            std::this_thread::sleep_for(1ms);
        }
    } catch (...) {
        result.error = std::current_exception();
    }
}

void run_async(cef_detail::media_input_exports_s& queue,
               gpu::texture_s&                    source,
               gpu::recording_context_s&          context,
               cef_wrapper::send_media_frame_t    api,
               const CefRefPtr<client_s>&         client,
               uint32_t                           inputs)
{
    transfer_result_s result;
    auto& [error, delivered, hold_us] = result;
    std::jthread transfer([&](const std::stop_token& stop) { transfer_frames(stop, queue, api, client, result); });
    const auto   start = std::chrono::steady_clock::now();
    try {
        for (int frame = 0; frame < 120; ++frame) {
            if (auto commands = context.try_record()) {
                for (uint32_t input = 0; input < inputs; ++input) {
                    commands->clear(source, input_color(input, frame >= 60));
                    if (auto publication = queue.record(input, *commands, source, int64_t(frame) * 16667)) {
                        commands->on_submitted([publication](const gpu::completion_s&) { publication->commit(); });
                    }
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
    if (error) {
        std::rethrow_exception(error);
    }

    uint64_t admitted{};
    uint64_t drops{};
    for (uint32_t input = 0; input < inputs; ++input) {
        admitted += queue.metrics(input).admitted;
        drops += queue.metrics(input).capacity_drops;
    }

    std::ranges::sort(hold_us);
    std::cout << "Async 60 Hz: admitted=" << admitted << " delivered=" << delivered << " capacity_drops=" << drops;
    if (!hold_us.empty()) {
        std::cout << "; send-to-reuse us p50=" << hold_us[(hold_us.size() - 1) / 2]
                  << " p95=" << hold_us[(hold_us.size() - 1) * 95 / 100] << " max=" << hold_us.back();
    }

    std::cout << '\n';
}
void run_serial(const std::vector<std::unique_ptr<gpu::detail::external_image_export_s>>& exports,
                gpu::texture_s&                                                           source,
                gpu::recording_context_s&                                                 context,
                cef_wrapper::send_media_frame_t                                           api,
                const CefRefPtr<client_s>&                                                client,
                uint32_t                                                                  inputs)
{
    gpu::draw_s conversion;
    conversion.compositing         = gpu::compositing_e::replace;
    conversion.transfer            = gpu::color_operation_e::encode_srgb_premultiplied;
    int                  delivered = 0;
    std::vector<int64_t> hold_us;
    for (int frame = 0; frame < 120; ++frame) {
        auto record = context.try_record();
        if (!record) {
            throw std::runtime_error("Export recording unavailable");
        }

        for (uint32_t input = 0; input < inputs; ++input) {
            record->clear(source, input_color(input, frame >= 60));
            exports[input]->copy(*record, source, conversion);
        }

        if (record->submit().wait(5s) != gpu::wait_result_e::ready) {
            throw std::runtime_error("Producer GPU completion failed");
        }

        record.reset();
        for (uint32_t input = 0; input < inputs; ++input) {
            const auto started = std::chrono::steady_clock::now();
            delivered += static_cast<int>(send(api, client, *exports[input], int64_t(frame) * 16667, input));
            hold_us.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count());
        }

        std::this_thread::sleep_for(16ms);
    }

    std::ranges::sort(hold_us);
    std::cout << "Delivered " << delivered << "/" << 120 * inputs << " across " << inputs
              << " inputs; send-to-reuse us p50=" << hold_us[(hold_us.size() / 2) - 1]
              << " p95=" << hold_us[(hold_us.size() * 95 / 100) - 1] << " max=" << hold_us.back() << '\n';
}

void check_generations(cef_wrapper::send_media_frame_t             api,
                       const CefRefPtr<client_s>&                  client,
                       const gpu::detail::external_image_export_s& exported,
                       bool                                        reject_invalid_import)
{
    // Metadata invalidation must reject a subsequently arriving old
    // frame without importing it or manufacturing a GPU completion.
    auto await = [](std::future<std::pair<int, int>> result) {
        if (result.wait_for(5s) != std::future_status::ready) {
            throw std::runtime_error("Generation qualification timed out");
        }

        const auto response = result.get();
        if (response.first == 0) {
            throw std::runtime_error("Generation qualification did not establish safe retirement");
        }

        return response.second;
    };

    auto invalidation              = cef_wrapper::make_media_frame();
    invalidation.source_generation = 2;
    invalidation.operation         = 1;
    if (await(enqueue(api, client, invalidation)) != 0) {
        throw std::runtime_error("Metadata invalidation delivered a video frame");
    }

    auto packet = describe(exported, 0, 2'100'000);
    if (await(enqueue(api, client, packet)) != 0) {
        throw std::runtime_error("Superseded source generation entered the media source");
    }

    packet.source_generation = 2;
    if (await(enqueue(api, client, packet)) == 0) {
        throw std::runtime_error("Current source generation was not admitted");
    }

    std::cout << "Metadata invalidation rejected the old generation and admitted its replacement\n";
    if (reject_invalid_import) {
        packet.modifier = UINT64_MAX;
        auto rejected   = enqueue(api, client, packet);
        if (rejected.wait_for(5s) == std::future_status::ready) {
            const auto [safe, accepted] = rejected.get();
            if (accepted != 0) {
                throw std::runtime_error("Failed image import was reported as delivered");
            }

            std::cout << "Invalid import rejected; safe retirement=" << safe << '\n';
            if (safe != 0) {
                client->wait_input_failure();
            }
        } else {
            // Some drivers reject backing creation by losing the GPU
            // channel. No query callback means no safe retirement.
            // The exporter outlives runtime shutdown; never reuse it.
            std::cout << "Invalid import lost completion; exporter retained until runtime shutdown\n";
        }
    }
}

void create_browser(const CefRefPtr<client_s>& client, uint32_t inputs)
{
    std::string page =
        R"HTML(
<!doctype html>
<style>
  html,
  body {
    margin: 0;
    width: 100%;
    height: 100%;
    background: transparent;
    overflow: hidden;
  }
  body {
    display: flex;
  }
  video {
    display: block;
    min-width: 0;
    flex: 1;
    width: 0;
    height: 100%;
    object-fit: fill;
  }
</style>
)HTML";
    for (uint32_t input = 0; input < inputs; ++input) {
        page += std::format("<video style=\"flex:none;width:{}px\" muted autoplay playsinline></video>",
                            (640 * (input + 1) / inputs) - (640 * input / inputs));
    }

    page +=
        R"HTML(
<script>
  globalThis.probeStats = [];
  document.querySelectorAll("video").forEach((v, inputIndex) =>
    miximus
      .getInputMediaStream({ inputIndex })
      .then(async (s) => {
        if ((await miximus.getInputMediaStream({ inputIndex })) !== s)
          throw Error("Stream identity changed");
        const t = s.getVideoTracks()[0],
          clone = t.clone();
        t.stop();
        const next = await miximus.getInputMediaStream({ inputIndex });
        if (
          next.getVideoTracks()[0].readyState !== "live" ||
          clone.readyState !== "live"
        )
          throw Error("Reacquisition stopped a live track");
        clone.stop();
        const stats = { callbacks: 0, presented: 0, lastMediaTime: 0 };
        probeStats[inputIndex] = stats;
        const observe = (now, m) => {
          stats.callbacks++;
          stats.presented = m.presentedFrames;
          stats.lastMediaTime = m.mediaTime;
          v.requestVideoFrameCallback(observe);
        };
        v.requestVideoFrameCallback(observe);
        v.srcObject = next;
        return v.play();
      })
      .catch((e) => console.error(String(e))),
  );
</script>
)HTML";
    auto create = [client, page] {
        CefWindowInfo window;
        window.SetAsWindowless(CefWindowHandle{});
        window.shared_texture_enabled = true;
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 60;
        settings.background_color      = CefColorSetARGB(0, 0, 0, 0);
        if (!CefBrowserHost::CreateBrowser(
                window, client, "data:text/html," + CefURIEncode(page, false).ToString(), settings, nullptr, nullptr)) {
            client->creation_failed();
        }
    };
    if (!CefPostTask(TID_UI, new cef_detail::task_s(std::move(create)))) {
        throw std::runtime_error("Cannot create probe browser");
    }
}

struct probe_options_s
{
    uint32_t inputs{1};
    uint32_t async_depth{};
    uint32_t square_size{};
    bool     reject_invalid_import{};
    bool     mipmaps{};
    bool     anisotropy{};

    gpu::extent_s export_size() const
    {
        if (anisotropy) {
            return {.width = 640, .height = 2880};
        }
        if (mipmaps) {
            return {.width = 2560, .height = 1440};
        }
        if (square_size != 0U) {
            return {.width = square_size, .height = square_size};
        }
        return {.width = 640, .height = 360};
    }
};

probe_options_s parse_options(int argc, char** argv)
{
    probe_options_s options;
    auto& [inputs, async_depth, square_size, reject_invalid_import, mipmaps, anisotropy] = options;
    if (argc >= 4) {
        const std::string_view value(argv[3]);
        const auto             parsed = std::from_chars(value.data(), value.data() + value.size(), inputs);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || inputs < 1 || inputs > 8) {
            throw std::runtime_error("INPUT_COUNT must be 1 through 8");
        }
    }

    if (argc == 5) {
        const std::string_view value(argv[4]);
        if (value == "--mipmaps" || value == "--anisotropy") {
            if (inputs != 1) {
                throw std::runtime_error("Mipmap comparison requires INPUT_COUNT=1");
            }
            mipmaps    = true;
            anisotropy = value == "--anisotropy";
        } else if (value == "--reject-invalid-import") {
            reject_invalid_import = true;
        } else if (value.starts_with("--size=")) {
            const auto size   = value.substr(7);
            const auto parsed = std::from_chars(size.data(), size.data() + size.size(), square_size);
            if (parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() || square_size < 1 ||
                square_size > 4096) {
                throw std::runtime_error("Size must be 1 through 4096");
            }
        } else {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), async_depth);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || async_depth < 1 ||
                async_depth > 8) {
                throw std::runtime_error("ASYNC_EXPORT_DEPTH must be 1 through 8");
            }
        }
    }

    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 3 || argc > 5) {
            std::cerr << "Usage: cef_media_input_probe RUNTIME_DIRECTORY PROFILE_DIRECTORY [INPUT_COUNT=1] "
                         "[ASYNC_EXPORT_DEPTH=1..8 | --reject-invalid-import | --size=N | --mipmaps | --anisotropy]\n";
            return 2;
        }

        std::cout.setf(std::ios::unitbuf);
        const auto probe_options = parse_options(argc, argv);
        const auto [inputs, async_depth, square_size, reject_invalid_import, mipmaps, anisotropy] = probe_options;

        logger::init_loggers(spdlog::level::warn);
        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        options.external_image_import = true;
        gpu::device_s                                                      gpu(options);
        const gpu::extent_s                                                export_size = probe_options.export_size();
        std::vector<std::unique_ptr<gpu::detail::external_image_export_s>> exports;
        for (uint32_t input = 0; (async_depth == 0U) && input < inputs; ++input) {
            exports.push_back(std::make_unique<gpu::detail::external_image_export_s>(gpu, export_size));
        }

        auto source  = gpu.create_texture(mipmaps ? export_size : gpu::extent_s{.width = 640, .height = 360});
        auto context = gpu.create_recording_context(3);
        cef_detail::media_input_exports_s::quarantine_s  quarantine;
        std::optional<cef_detail::media_input_exports_s> export_queue;
        if (async_depth != 0U) {
            export_queue.emplace(gpu, async_depth, quarantine);
            for (uint32_t input = 0; input < inputs; ++input) {
                if (!export_queue->configure(input, {.width = 640, .height = 360})) {
                    throw std::runtime_error("Cannot configure export queue");
                }
            }
        }

        // Runtime shuts down before exporter destruction, including failure paths.
        cef_detail::runtime_s runtime(argv[1], argv[2]);
        const auto            api = cef_wrapper::find_send_media_frame();
        if (api == nullptr) {
            throw std::runtime_error("Runtime does not provide the experimental media-input bridge");
        }

        CefRefPtr<client_s> client = new client_s(gpu, inputs);
        create_browser(client, inputs);

        try {
            client->wait_ready();
            if (anisotropy) {
                run_anisotropy(gpu, *exports.at(0), source, context, api, client);
            } else if (mipmaps) {
                run_mipmaps(gpu, *exports.at(0), source, context, api, client);
            } else if (export_queue) {
                run_async(*export_queue, source, context, api, client, inputs);
            } else {
                run_serial(exports, source, context, api, client, inputs);
            }

            if (!mipmaps) {
                client->wait_colors();
            }
            client->report_video_frames();
            if (!export_queue && !mipmaps) {
                check_generations(api, client, *exports.at(0), reject_invalid_import);
            }
        } catch (...) {
            client->close();
            throw;
        }

        client->close();
        if (gpu.validation_errors() != 0U) {
            throw std::runtime_error("Vulkan validation errors");
        }

        std::cout << "GPU-only input and accelerated output comparisons passed\n";
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("CEF probe failed with an unknown exception\n", stderr);
        return 1;
    }
    return 0;
}
