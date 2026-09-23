#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/device.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_task.h"
#include "logger/logger.hpp"
#include "nodes/cef/detail/runtime.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <mutex>
#include <string_view>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace miximus;

void log_producer_fence(int dma_buf)
{
    dma_buf_export_sync_file exported{};
    exported.flags = DMA_BUF_SYNC_READ;
    exported.fd    = -1;
    if (ioctl(dma_buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF producer fence");
    }
    struct owned_fd_s
    {
        int value;
        explicit owned_fd_s(int fd)
            : value(fd)
        {
        }
        owned_fd_s(const owned_fd_s& other)            = delete;
        owned_fd_s& operator=(const owned_fd_s& other) = delete;
        owned_fd_s(owned_fd_s&& other)                 = delete;
        owned_fd_s& operator=(owned_fd_s&& other)      = delete;
        ~owned_fd_s() { close(value); }
    } fence{exported.fd};
    sync_file_info info{};
    if (ioctl(fence.value, SYNC_IOC_FILE_INFO, &info) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF sync-file metadata");
    }
    // Metadata only: this does not map or read any image memory. A signalled
    // snapshot alone cannot prove that every producer write was published.
    std::cout << "CEF producer sync-file: fences=" << info.num_fences << " status=" << info.status << '\n';
    if (info.num_fences > 64) {
        throw std::runtime_error("Unexpected producer fence count");
    }
    std::vector<sync_fence_info> fences(info.num_fences);
    info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
    if (ioctl(fence.value, SYNC_IOC_FILE_INFO, &info) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF fence identities");
    }
    for (const auto& entry : fences) {
        std::cout << "Producer fence: driver=" << entry.driver_name << " timeline=" << entry.obj_name
                  << " status=" << entry.status << " timestamp_ns=" << entry.timestamp_ns << '\n';
    }
}

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
    gpu::extent_s            dimensions_;
    bool                     fence_logged_{};
    IMPLEMENT_REFCOUNTING(client_s);

    std::mutex              mutex;
    std::condition_variable changed;
    CefRefPtr<CefBrowser>   browser;
    bool                    received{};
    bool                    closed{};
    bool                    close_requested{};
    size_t                  copied_frames{};
    uint64_t                last_timestamp{};
    uint64_t                first_timestamp{};
    std::string             error;

  public:
    static constexpr size_t required_frames = 120;

    client_s(gpu::device_s& device, gpu::extent_s dimensions)
        : destination_(device.create_texture(dimensions))
        , context_(device.create_recording_context(1))
        , dimensions_(dimensions)
    {
    }
    void creation_failed()
    {
        {
            std::scoped_lock lock(mutex);
            closed = true;
        }
        fail("CEF browser creation rejected");
    }

    bool wait_for_capture()
    {
        std::unique_lock lock(mutex);
        const bool       signalled = changed.wait_for(lock, 15s, [&] { return received || !error.empty(); });
        const bool       success   = signalled && received && error.empty();
        if (!success) {
            std::cerr << "Accelerated probe failed: " << (error.empty() ? "insufficient accelerated frames" : error)
                      << " (completed " << copied_frames << '/' << required_frames << ")\n";
        } else {
            std::cout << "Completed " << copied_frames << " accelerated GPU copies; capture timestamps "
                      << first_timestamp << ".." << last_timestamp << " us\n";
        }
        return success;
    }

    bool wait_for_close(bool success)
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 10s, [&] { return closed; })) {
            std::terminate();
        }
        if (success && !error.empty()) {
            std::cerr << "Accelerated probe failed during closure: " << error << '\n';
            success = false;
        }
        return success;
    }

    CefRefPtr<CefRenderHandler>   GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    void                          GetViewRect(CefRefPtr<CefBrowser> /* browser */, CefRect& rect) override
    {
        rect = {0, 0, static_cast<int>(dimensions_.width), static_cast<int>(dimensions_.height)};
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> created) override
    {
        bool closing{};
        {
            std::scoped_lock lock(mutex);
            browser = created;
            closing = close_requested;
            std::cout << "CEF browser created\n";
            changed.notify_all();
        }
        if (closing) {
            created->GetHost()->CloseBrowser(true);
        }
    }
    void request_close()
    {
        CefRefPtr<CefBrowser> current;
        {
            std::scoped_lock lock(mutex);
            close_requested = true;
            current         = browser;
        }
        if (current) {
            current->GetHost()->CloseBrowser(true);
        }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser> /* browser */) override
    {
        std::scoped_lock lock(mutex);
        browser = nullptr;
        closed  = true;
        changed.notify_all();
    }
    void fail(std::string message)
    {
        std::scoped_lock lock(mutex);
        error = std::move(message);
        changed.notify_all();
    }
    void OnPaint(CefRefPtr<CefBrowser> /* browser */,
                 PaintElementType /* type */,
                 const RectList& /* dirty_rects */,
                 const void* /* buffer */,
                 int /* width */,
                 int /* height */) override
    {
        fail("CEF delivered software paint; no pixels were ingested");
    }
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> /* browser */,
                            PaintElementType type,
                            const RectList& /* dirty_rects */,
                            const CefAcceleratedPaintInfo& info) override
    {
        if (type != PET_VIEW) {
            return;
        }
        try {
            if (info.plane_count != 1 ||
                (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) {
                throw std::runtime_error("Unsupported accelerated descriptor format/planes");
            }
            gpu::detail::dma_buf_image_s source;
            source.fd     = info.planes[0].fd;
            source.extent = {.width  = static_cast<uint32_t>(info.extra.coded_size.width),
                             .height = static_cast<uint32_t>(info.extra.coded_size.height)};
            source.order =
                info.format == CEF_COLOR_TYPE_BGRA_8888 ? gpu::channel_order_e::bgra : gpu::channel_order_e::rgba;
            source.modifier = info.modifier;
            source.offset   = info.planes[0].offset;
            source.stride   = info.planes[0].stride;
            if (!fence_logged_ || copied_frames == required_frames - 1) {
                log_producer_fence(source.fd);
                fence_logged_ = true;
            }
            auto recording = context_.try_record();
            if (!recording) {
                throw gpu::recording_unavailable_s{};
            }
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
            const auto complete =
                gpu::detail::dma_buf_copy_s::submit(*recording, source, destination_, conversion, 100ms);
            // The probe process has an external timeout. Never return this borrow
            // merely because one completion wait timed out.
            while (complete.wait(1s) != gpu::wait_result_e::ready) {
            }
            std::scoped_lock lock(mutex);
            if (copied_frames == 0) {
                first_timestamp = info.extra.timestamp;
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
    if (argc != 3 && argc != 5) {
        return 2;
    }
    std::cout.setf(std::ios::unitbuf);
    logger::init_loggers(spdlog::level::warn);
    try {
        gpu::extent_s dimensions{.width = 640, .height = 360};
        if (argc == 5) {
            auto parse_dimension = [](std::string_view text) {
                uint32_t   value{};
                const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 || value > 8192) {
                    throw std::invalid_argument("Probe dimensions must be between 1 and 8192");
                }
                return value;
            };
            dimensions = {.width = parse_dimension(argv[3]), .height = parse_dimension(argv[4])};
        }
        gpu::device_options_s options;
        options.external_image_import = true;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        gpu::device_s device(options);
        bool          success{};
        {
            nodes::cef::detail::runtime_s runtime(argv[1], argv[2]);
            std::cout << "CEF runtime initialized; creating accelerated browser\n";
            CefRefPtr<client_s> client = new client_s(device, dimensions);
            if (!CefPostTask(TID_UI, new task_s([client] {
                                 CefWindowInfo window;
                                 window.SetAsWindowless(0);
                                 window.shared_texture_enabled = 1;
                                 CefBrowserSettings settings;
                                 settings.windowless_frame_rate = 60;
                                 const char* url =
                                     "data:text/html,<html><style>"
                                     "@keyframes move{from{transform:translateX(0px)}to{transform:translateX(220px)}}"
                                     "</style><body style='background:transparent'><div "
                                     "style='background:rgba(128,64,32,0.5);width:200px;height:200px;"
                                     "animation:move 1s linear infinite alternate'></div></body></html>";
                                 if (!CefBrowserHost::CreateBrowser(window, client, url, settings, nullptr, nullptr)) {
                                     client->creation_failed();
                                 }
                             }))) {
                throw std::runtime_error("Cannot dispatch browser creation");
            }
            success = client->wait_for_capture();
            // Creation may still be pending when capture times out. Keep the client
            // alive and close even a browser that arrives after this request.
            if (!CefPostTask(TID_UI, new task_s([client] { client->request_close(); }))) {
                std::terminate();
            }
            success = client->wait_for_close(success);
            client  = nullptr;
        }
        // Include errors emitted while CEF shuts down, with the Vulkan device
        // still alive and its validation callback installed.
        return success && device.validation_errors() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
