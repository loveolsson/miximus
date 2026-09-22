#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/device.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_task.h"
#include "logger/logger.hpp"
#include "nodes/cef/detail/runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>

namespace {
using namespace std::chrono_literals;
using namespace miximus;

class task_s final : public CefTask
{
    std::function<void()> function_;
    IMPLEMENT_REFCOUNTING(task_s);

  public:
    explicit task_s(std::function<void()> function)
        : function_(std::move(function))
    {
    }
    void Execute() override { function_(); }
};

class client_s final
    : public CefClient
    , public CefRenderHandler
    , public CefLifeSpanHandler
{
    gpu::texture_s           destination_;
    gpu::recording_context_s context_;
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    std::mutex              mutex;
    std::condition_variable changed;
    CefRefPtr<CefBrowser>   browser;
    bool                    received{};
    bool                    closed{};
    bool                    close_requested{};
    size_t                  copied_frames{};
    uint64_t                last_timestamp{};
    std::string             error;

    static constexpr size_t required_frames = 120;

    explicit client_s(gpu::device_s& device)
        : destination_(device.create_texture({640, 360}))
        , context_(device.create_recording_context(1))
    {
    }
    CefRefPtr<CefRenderHandler>   GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = {0, 0, 640, 360}; }
    void OnAfterCreated(CefRefPtr<CefBrowser> created) override
    {
        bool closing;
        {
            std::lock_guard lock(mutex);
            browser = created;
            closing = close_requested;
            std::cout << "CEF browser created\n";
            changed.notify_all();
        }
        if (closing)
            created->GetHost()->CloseBrowser(true);
    }
    void request_close()
    {
        CefRefPtr<CefBrowser> current;
        {
            std::lock_guard lock(mutex);
            close_requested = true;
            current         = browser;
        }
        if (current)
            current->GetHost()->CloseBrowser(true);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        std::lock_guard lock(mutex);
        browser = nullptr;
        closed  = true;
        changed.notify_all();
    }
    void fail(std::string message)
    {
        std::lock_guard lock(mutex);
        error = std::move(message);
        changed.notify_all();
    }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override
    {
        fail("CEF delivered software paint; no pixels were ingested");
    }
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>,
                            PaintElementType type,
                            const RectList&,
                            const CefAcceleratedPaintInfo& info) override
    {
        if (type != PET_VIEW)
            return;
        try {
            if (info.plane_count != 1 ||
                (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) {
                throw std::runtime_error("Unsupported accelerated descriptor format/planes");
            }
            gpu::detail::dma_buf_image_s source;
            source.fd     = info.planes[0].fd;
            source.extent = {static_cast<uint32_t>(info.extra.coded_size.width),
                             static_cast<uint32_t>(info.extra.coded_size.height)};
            source.order =
                info.format == CEF_COLOR_TYPE_BGRA_8888 ? gpu::channel_order_e::bgra : gpu::channel_order_e::rgba;
            source.modifier = info.modifier;
            source.offset   = info.planes[0].offset;
            source.stride   = info.planes[0].stride;
            auto recording  = context_.try_record();
            if (!recording)
                throw gpu::recording_unavailable_s{};
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
            const auto complete =
                gpu::detail::dma_buf_copy_s::submit(*recording, source, destination_, conversion, 100ms);
            // The probe process has an external timeout. Never return this borrow
            // merely because one completion wait timed out.
            while (complete.wait(1s) != gpu::wait_result_e::ready) {
            }
            std::lock_guard lock(mutex);
            if (copied_frames == 0) {
                std::cout << "Accelerated GPU copy: " << source.extent.width << 'x' << source.extent.height
                          << " modifier=" << source.modifier << " timestamp=" << info.extra.timestamp << '\n';
            }
            if (copied_frames != 0 && info.extra.timestamp < last_timestamp) {
                error = "Accelerated capture timestamps moved backwards";
            }
            last_timestamp = info.extra.timestamp;
            received       = ++copied_frames >= required_frames;
            changed.notify_all();
        } catch (const std::exception& failure) {
            fail(failure.what());
        }
    }
};
} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
        return 2;
    std::cout.setf(std::ios::unitbuf);
    logger::init_loggers(spdlog::level::warn);
    try {
        gpu::device_options_s options;
        options.external_image_import = true;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        gpu::device_s                 device(options);
        nodes::cef::detail::runtime_s runtime(argv[1], argv[2]);
        std::cout << "CEF runtime initialized; creating accelerated browser\n";
        CefRefPtr<client_s> client = new client_s(device);
        if (!CefPostTask(TID_UI, new task_s([client] {
                             CefWindowInfo window;
                             window.SetAsWindowless(0);
                             window.shared_texture_enabled = true;
                             CefBrowserSettings settings;
                             settings.windowless_frame_rate = 60;
                             const char* url =
                                 "data:text/html,<html><style>"
                                 "@keyframes move{from{transform:translateX(0px)}to{transform:translateX(220px)}}"
                                 "</style><body style='background:transparent'><div "
                                 "style='background:rgba(128,64,32,0.5);width:200px;height:200px;"
                                 "animation:move 1s linear infinite alternate'></div></body></html>";
                             if (!CefBrowserHost::CreateBrowser(window, client, url, settings, nullptr, nullptr)) {
                                 {
                                     std::lock_guard lock(client->mutex);
                                     client->closed = true;
                                 }
                                 client->fail("CEF browser creation rejected");
                             }
                         })))
            throw std::runtime_error("Cannot dispatch browser creation");
        std::unique_lock lock(client->mutex);
        const bool       signalled =
            client->changed.wait_for(lock, 15s, [&] { return client->received || !client->error.empty(); });
        const bool success = signalled && client->received && client->error.empty();
        if (!success)
            std::cerr << "Accelerated probe failed: "
                      << (client->error.empty() ? "insufficient accelerated frames" : client->error) << " (completed "
                      << client->copied_frames << '/' << client_s::required_frames << ")\n";
        else
            std::cout << "Completed " << client->copied_frames << " accelerated GPU copies\n";
        lock.unlock();
        // Creation may still be pending when capture times out. Keep the client
        // alive and close even a browser that arrives after this request.
        if (!CefPostTask(TID_UI, new task_s([client] { client->request_close(); })))
            std::terminate();
        lock.lock();
        if (!client->changed.wait_for(lock, 10s, [&] { return client->closed; }))
            std::terminate();
        lock.unlock();
        client = nullptr;
        return success && device.validation_errors() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
