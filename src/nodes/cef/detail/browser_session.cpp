#include "browser_session.hpp"

#include "gpu/detail/dma_buf_copy.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_task.h"

#include <atomic>
#include <condition_variable>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace miximus::nodes::cef::detail {
namespace {
using namespace std::chrono_literals;
using phase_e       = browser_session_s::phase_e;
using frame_queue_t = media::timed_source_queue_s<browser_session_s::frame_ptr_t>;

constexpr size_t FRAME_CAPACITY = 8;
constexpr size_t FRAME_BUDGET   = 1024ULL * 1024 * 1024;

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

struct shared_state_s
{
    std::atomic<phase_e>            phase{phase_e::starting};
    std::atomic_bool                started{};
    std::atomic_bool                close_requested{};
    std::atomic_bool                closed{};
    std::atomic_uint64_t            received{};
    std::atomic_uint64_t            copied{};
    std::atomic_uint64_t            dropped{};
    mutable std::mutex              mutex;
    mutable std::condition_variable changed;
    std::string                     error;
    frame_queue_t                   frames{
                          {.capacity = 4, .playout_delay_frames = 1}
    };

    void fail(std::string message)
    {
        std::lock_guard lock(mutex);
        if (error.empty())
            error = std::move(message);
        phase = phase_e::failed;
    }

    void mark_closed()
    {
        {
            std::lock_guard lock(mutex);
            closed = true;
            phase  = error.empty() ? phase_e::closed : phase_e::failed;
        }
        changed.notify_all();
    }
};

// Browser references, recording context and capture timestamps belong to CEF's
// UI thread after construction. The shared queue has its existing producer lock.
class client_s final
    : public CefClient
    , public CefRenderHandler
    , public CefLifeSpanHandler
    , public CefLoadHandler
    , public CefRequestHandler
    , public CefPermissionHandler
    , public CefDownloadHandler
{
    std::shared_ptr<shared_state_s> state_;
    browser_session_s::options_s    options_;
    frame_pool_s                    pool_;
    gpu::recording_context_s        context_;
    CefRefPtr<CefBrowser>           browser_;
    bool                            creation_pending_{};
    uint64_t                        epoch_{1};
    std::optional<uint64_t>         previous_timestamp_;
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    client_s(gpu::device_s& device, browser_session_s::options_s options, std::shared_ptr<shared_state_s> state)
        : state_(std::move(state))
        , options_(std::move(options))
        , pool_(device, options_.dimensions, FRAME_CAPACITY, FRAME_BUDGET)
        , context_(device.create_recording_context(1))
    {
    }

    CefRefPtr<CefRenderHandler>     GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler>   GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>       GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler>    GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDownloadHandler>   GetDownloadHandler() override { return this; }

    bool resources_idle() const { return pool_.idle(); }

    void start()
    {
        if (state_->close_requested) {
            state_->mark_closed();
            return;
        }
        CefWindowInfo window;
        window.SetAsWindowless(0);
        window.shared_texture_enabled = true;
        CefBrowserSettings settings;
        settings.windowless_frame_rate = options_.frame_rate;
        settings.background_color      = CefColorSetARGB(0, 0, 0, 0);
        creation_pending_ = CefBrowserHost::CreateBrowser(window, this, options_.url, settings, nullptr, nullptr);
        if (!creation_pending_) {
            state_->fail("CEF rejected browser creation");
            state_->mark_closed();
        }
    }

    void close()
    {
        if (browser_)
            browser_->GetHost()->CloseBrowser(true);
        else if (!creation_pending_)
            state_->mark_closed();
    }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        rect = {0, 0, options_.dimensions.x, options_.dimensions.y};
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        creation_pending_ = false;
        browser_          = browser;
        browser_->GetHost()->SetAudioMuted(true);
        if (state_->close_requested)
            close();
        else
            state_->phase = phase_e::loading;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        browser_ = nullptr;
        state_->mark_closed();
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser>,
                       CefRefPtr<CefFrame>,
                       int,
                       const CefString&,
                       const CefString&,
                       WindowOpenDisposition,
                       bool,
                       const CefPopupFeatures&,
                       CefWindowInfo&,
                       CefRefPtr<CefClient>&,
                       CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&,
                       bool*) override
    {
        return true;
    }

    bool CanDownload(CefRefPtr<CefBrowser>, const CefString&, const CefString&) override { return false; }

    bool OnBeforeDownload(CefRefPtr<CefBrowser>,
                          CefRefPtr<CefDownloadItem>,
                          const CefString&,
                          CefRefPtr<CefBeforeDownloadCallback>) override
    {
        return true; // Do not continue the download or show Chrome's download UI.
    }

    void OnDownloadUpdated(CefRefPtr<CefBrowser>,
                           CefRefPtr<CefDownloadItem>,
                           CefRefPtr<CefDownloadItemCallback> callback) override
    {
        callback->Cancel();
    }

    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>,
                                        CefRefPtr<CefFrame>,
                                        const CefString&,
                                        uint32_t,
                                        CefRefPtr<CefMediaAccessCallback> callback) override
    {
        callback->Continue(0);
        return true;
    }

    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>,
                                uint64_t,
                                const CefString&,
                                uint32_t,
                                CefRefPtr<CefPermissionPromptCallback> callback) override
    {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return true;
    }

    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override
    {
        if (frame->IsMain() && !state_->close_requested) {
            ++epoch_;
            previous_timestamp_.reset();
            state_->phase = phase_e::loading;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode           code,
                     const CefString&    text,
                     const CefString&) override
    {
        if (frame->IsMain() && code != ERR_ABORTED && !state_->close_requested)
            state_->fail(std::format("CEF load error {}: {}", static_cast<int>(code), text.ToString()));
    }

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int code, const CefString& text) override
    {
        state_->fail(std::format("CEF renderer terminated {}: {}", code, text.ToString()));
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override
    {
        state_->fail("CEF delivered software paint; accelerated rendering is required");
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser>,
                            PaintElementType type,
                            // Always copy the full frame; damage does not change capture work.
                            const RectList&,
                            const CefAcceleratedPaintInfo& info) override
    {
        if (state_->close_requested || state_->phase == phase_e::failed)
            return;
        const auto arrival  = utils::flicks_now();
        const auto sequence = ++state_->received;
        try {
            // Popup composition is a subsequent contained session step. Never
            // silently publish an incomplete browser image or ingest CPU paint.
            if (type != PET_VIEW)
                throw std::runtime_error("CEF popup composition is not available yet");
            if (info.plane_count != 1 ||
                (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888))
                throw std::runtime_error("Unsupported CEF accelerated format or plane count");
            if (info.extra.coded_size.width != options_.dimensions.x ||
                info.extra.coded_size.height != options_.dimensions.y)
                throw std::runtime_error("CEF paint does not match the session viewport generation");
            const auto maximum_timestamp =
                std::chrono::duration_cast<std::chrono::microseconds>(utils::flicks::max()).count();
            if (info.extra.timestamp > static_cast<uint64_t>(maximum_timestamp))
                throw std::runtime_error("CEF capture timestamp is outside the supported range");
            if (previous_timestamp_ && info.extra.timestamp < *previous_timestamp_)
                ++epoch_;
            previous_timestamp_ = info.extra.timestamp;

            auto frame     = pool_.try_acquire();
            auto recording = frame ? context_.try_record() : nullptr;
            if (!frame || !recording) {
                ++state_->dropped;
                return;
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
            gpu::draw_s conversion;
            conversion.compositing = gpu::compositing_e::replace;
            conversion.transfer    = gpu::color_operation_e::decode_srgb_premultiplied;
            const auto completion =
                gpu::detail::dma_buf_copy_s::submit(*recording, source, frame->texture(), conversion, 100ms);
            // A timeout does not cancel GPU work. The borrowed source cannot be
            // returned while our read is pending; the app's shutdown watchdog
            // handles a device that stops making progress.
            while (completion.wait(1s) != gpu::wait_result_e::ready) {
            }
            if (state_->close_requested)
                return;
            const media::media_clock_sample_s clock{
                .stream_epoch   = epoch_,
                .frame_sequence = sequence,
                .media_pts      = utils::flicks_cast(std::chrono::microseconds(info.extra.timestamp)),
                .frame_duration = utils::k_flicks_one_second / options_.frame_rate,
            };
            state_->frames.push(std::make_shared<frame_queue_t::frame_t>(
                clock, arrival, std::move(frame), media::source_frame_readiness_e::ready));
            ++state_->copied;
            state_->phase = phase_e::ready;
        } catch (const gpu::recording_unavailable_s&) {
            ++state_->dropped;
        } catch (const std::exception& failure) {
            state_->fail(failure.what());
        }
    }
};
} // namespace

struct browser_session_s::impl_s
{
    std::shared_ptr<shared_state_s>        state = std::make_shared<shared_state_s>();
    CefRefPtr<client_s>                    client;
    std::optional<frame_queue_t::ticket_t> prepared;
};

browser_session_s::browser_session_s(gpu::device_s& device, options_s options)
    : impl_(std::make_unique<impl_s>())
{
    if (!MIXIMUS_CEF_NATIVE_CAPTURE_READY)
        throw std::runtime_error("CEF sessions require the verified native-handle completion SDK");
    // CEF 152 has no historical 60 fps ceiling; its microsecond period must
    // remain nonzero. Actual delivery can be slower than the requested rate.
    if (options.url.empty() || options.dimensions.x < 1 || options.dimensions.y < 1 || options.dimensions.x > 8192 ||
        options.dimensions.y > 8192 || options.frame_rate < 1 || options.frame_rate > 1'000'000)
        throw std::invalid_argument("Invalid CEF session options");
    impl_->client = new client_s(device, std::move(options), impl_->state);
}

browser_session_s::~browser_session_s() { close_async(); }

void browser_session_s::start_async()
{
    if (impl_->state->started.exchange(true))
        return;
    if (!CefPostTask(TID_UI, new task_s([client = impl_->client] { client->start(); }))) {
        impl_->state->fail("Cannot dispatch CEF browser creation");
        impl_->state->mark_closed();
    }
}

void browser_session_s::close_async()
{
    if (impl_->state->closed || impl_->state->close_requested.exchange(true))
        return;
    impl_->state->phase = phase_e::closing;
    if (!CefPostTask(TID_UI, new task_s([client = impl_->client] { client->close(); })))
        impl_->state->fail("Cannot dispatch CEF browser closure");
}

bool browser_session_s::closed() const noexcept { return impl_->state->closed; }

size_t browser_session_s::texture_budget(const options_s& options)
{
    if (options.dimensions.x < 1 || options.dimensions.y < 1 || options.dimensions.x > 8192 ||
        options.dimensions.y > 8192)
        throw std::invalid_argument("Invalid CEF viewport dimensions");
    const auto bytes = gpu::texture_s::estimate_storage_byte_size(
        options.dimensions, gpu::format_e::rgba_unorm16, gpu::sampling_e::linear);
    if (bytes == 0 || bytes > FRAME_BUDGET / FRAME_CAPACITY)
        throw std::invalid_argument("CEF viewport exceeds the session texture budget");
    return bytes * FRAME_CAPACITY;
}

bool browser_session_s::resources_idle() const { return impl_->client->resources_idle(); }

bool browser_session_s::wait_closed(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(impl_->state->mutex);
    return impl_->state->changed.wait_for(lock, timeout, [&] { return closed(); });
}

void browser_session_s::advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity)
{
    impl_->state->frames.advance(pts, target_time, discontinuity);
}

bool browser_session_s::submit_frame(utils::flicks pts)
{
    impl_->prepared.emplace(impl_->state->frames.select_nearest(pts));
    return impl_->prepared->frame() != nullptr;
}

browser_session_s::frame_ptr_t browser_session_s::resolve_frame()
{
    if (!impl_->prepared || !impl_->prepared->frame())
        return {};
    const auto& ticket = *impl_->prepared;
    if (!ticket.await() || !impl_->state->frames.commit(ticket))
        return {};
    return ticket.frame()->payload();
}

void browser_session_s::release_prepared_frame() { impl_->prepared.reset(); }
void browser_session_s::reset_frames()
{
    release_prepared_frame();
    impl_->state->frames.reset();
}

browser_session_s::metrics_s browser_session_s::metrics() const
{
    auto&           state = *impl_->state;
    std::lock_guard lock(state.mutex);
    return {.phase        = state.phase.load(),
            .error        = state.error,
            .received     = state.received.load(),
            .copied       = state.copied.load(),
            .dropped      = state.dropped.load(),
            .source_queue = state.frames.metrics()};
}

} // namespace miximus::nodes::cef::detail
