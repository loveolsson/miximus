#include "browser_session.hpp"

#include "capture_stream.hpp"
#include "command_channel.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_jsdialog_handler.h"
#include "task.hpp"

#include <atomic>
#include <condition_variable>
#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace miximus::nodes::cef {
using namespace detail;
namespace {
using namespace std::chrono_literals;
using phase_e = browser_session_s::phase_e;

struct shared_state_s
{
    std::shared_ptr<command_channel_s> commands = std::make_shared<command_channel_s>();
    std::atomic<phase_e>               phase{phase_e::starting};
    std::atomic_bool                   started;
    std::atomic_bool                   close_requested;
    std::atomic_bool                   closed;
    mutable std::mutex                 mutex;
    mutable std::condition_variable    changed;
    std::string                        error;
    void                               fail(std::string message)
    {
        std::scoped_lock lock(mutex);
        if (error.empty()) {
            error = std::move(message);
        }
        phase = phase_e::failed;
    }

    void mark_closed()
    {
        commands->close();
        {
            std::scoped_lock lock(mutex);
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
    , public CefJSDialogHandler
    , public CefDialogHandler
    , public CefContextMenuHandler
{
    std::shared_ptr<shared_state_s> state_;
    browser_session_s::options_s    options_;
    capture_stream_s                capture_;
    CefRefPtr<CefBrowser>           browser_;
    bool                            creation_pending_{};
    IMPLEMENT_REFCOUNTING(client_s);

  public:
    client_s(gpu::device_s& device, browser_session_s::options_s options, std::shared_ptr<shared_state_s> state)
        : state_(std::move(state))
        , options_(std::move(options))
        , capture_(device, options_.dimensions, options_.frame_rate)
    {
    }

    CefRefPtr<CefRenderHandler>     GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler>   GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler>       GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler>    GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDownloadHandler>   GetDownloadHandler() override { return this; }

    CefRefPtr<CefJSDialogHandler>    GetJSDialogHandler() override { return this; }
    CefRefPtr<CefDialogHandler>      GetDialogHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

    capture_stream_s& capture() { return capture_; }

    void start()
    {
        if (state_->close_requested) {
            state_->mark_closed();
            return;
        }
        CefWindowInfo window;
        window.SetAsWindowless(0);
        window.shared_texture_enabled = 1;
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
        state_->commands->cancel_commands("Browser is closing");
        if (browser_) {
            browser_->GetHost()->CloseBrowser(true);
        } else if (!creation_pending_) {
            state_->mark_closed();
        }
    }

    void GetViewRect(CefRefPtr<CefBrowser> /* browser */, CefRect& rect) override
    {
        rect = {0, 0, options_.dimensions.x, options_.dimensions.y};
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        creation_pending_ = false;
        browser_          = browser;
        state_->commands->attach(browser);
        browser_->GetHost()->SetAudioMuted(true);
        if (state_->close_requested) {
            close();
        } else {
            state_->phase = phase_e::loading;
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> /* browser */) override
    {
        browser_ = nullptr;
        state_->commands->detach();
        state_->mark_closed();
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser> /* browser */,
                       CefRefPtr<CefFrame> /* frame */,
                       int /* popup_id */,
                       const CefString& /* target_url */,
                       const CefString& /* target_frame_name */,
                       WindowOpenDisposition /* target_disposition */,
                       bool /* user_gesture */,
                       const CefPopupFeatures& /* popup_features */,
                       CefWindowInfo& /* window_info */,
                       CefRefPtr<CefClient>& /* client */,
                       CefBrowserSettings& /* settings */,
                       CefRefPtr<CefDictionaryValue>& /* extra_info */,
                       bool* /* no_javascript_access */) override
    {
        return true;
    }

    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> /* browser */,
                          CefRefPtr<CefFrame> /* frame */,
                          const CefString& /* target_url */,
                          WindowOpenDisposition disposition,
                          bool /* user_gesture */) override
    {
        return disposition != CEF_WOD_CURRENT_TAB;
    }

    bool OnJSDialog(CefRefPtr<CefBrowser> /* browser */,
                    const CefString& /* origin_url */,
                    JSDialogType /* dialog_type */,
                    const CefString& /* message_text */,
                    const CefString& /* default_prompt_text */,
                    CefRefPtr<CefJSDialogCallback> /* callback */,
                    bool& suppress) override
    {
        // CEF's suppression path continues script execution without a modal UI.
        suppress = true;
        return false;
    }

    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> /* browser */,
                              const CefString& /* message_text */,
                              bool /* is_reload */,
                              CefRefPtr<CefJSDialogCallback> callback) override
    {
        // Page content cannot veto a requested navigation/reload or shutdown.
        callback->Continue(true, {});
        return true;
    }

    bool OnFileDialog(CefRefPtr<CefBrowser> /* browser */,
                      FileDialogMode /* mode */,
                      const CefString& /* title */,
                      const CefString& /* default_file_path */,
                      const std::vector<CefString>& /* accept_filters */,
                      const std::vector<CefString>& /* accept_extensions */,
                      const std::vector<CefString>& /* accept_descriptions */,
                      CefRefPtr<CefFileDialogCallback> callback) override
    {
        callback->Cancel();
        return true;
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> /* browser */,
                             CefRefPtr<CefFrame> /* frame */,
                             CefRefPtr<CefContextMenuParams> /* params */,
                             CefRefPtr<CefMenuModel> model) override
    {
        model->Clear();
    }

    bool CanDownload(CefRefPtr<CefBrowser> /* browser */,
                     const CefString& /* url */,
                     const CefString& /* request_method */) override
    {
        return false;
    }

    bool OnBeforeDownload(CefRefPtr<CefBrowser> /* browser */,
                          CefRefPtr<CefDownloadItem> /* download_item */,
                          const CefString& /* suggested_name */,
                          CefRefPtr<CefBeforeDownloadCallback> /* callback */) override
    {
        return true; // Do not continue the download or show Chrome's download UI.
    }

    void OnDownloadUpdated(CefRefPtr<CefBrowser> /* browser */,
                           CefRefPtr<CefDownloadItem> /* download_item */,
                           CefRefPtr<CefDownloadItemCallback> callback) override
    {
        callback->Cancel();
    }

    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> /* browser */,
                                        CefRefPtr<CefFrame> /* frame */,
                                        const CefString& /* requesting_origin */,
                                        uint32_t /* requested_permissions */,
                                        CefRefPtr<CefMediaAccessCallback> callback) override
    {
        callback->Continue(0);
        return true;
    }

    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> /* browser */,
                                uint64_t /* prompt_id */,
                                const CefString& /* requesting_origin */,
                                uint32_t /* requested_permissions */,
                                CefRefPtr<CefPermissionPromptCallback> callback) override
    {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return true;
    }

    void OnLoadStart(CefRefPtr<CefBrowser> /* browser */,
                     CefRefPtr<CefFrame> frame,
                     TransitionType /* transition_type */) override
    {
        if (frame->IsMain() && !state_->close_requested) {
            state_->commands->cancel_commands("Browser navigated");
            capture_.new_epoch();
            state_->phase = phase_e::loading;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser> /* browser */,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode           code,
                     const CefString&    text,
                     const CefString& /* failed_url */) override
    {
        if (frame->IsMain() && code != ERR_ABORTED && !state_->close_requested) {
            state_->fail(std::format("CEF load error {}: {}", static_cast<int>(code), text.ToString()));
        }
    }

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> /* browser */,
                                   TerminationStatus /* status */,
                                   int              code,
                                   const CefString& text) override
    {
        state_->commands->cancel_commands("Browser renderer terminated");
        state_->fail(std::format("CEF renderer terminated {}: {}", code, text.ToString()));
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /* browser */,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        return state_->commands->receive(frame, source, message);
    }

    void OnPaint(CefRefPtr<CefBrowser> /* browser */,
                 PaintElementType type,
                 const RectList& /* dirty_rects */,
                 const void* /* buffer */,
                 int /* width */,
                 int /* height */) override
    {
        if (type == PET_POPUP) {
            return;
        }
        state_->fail("CEF delivered software paint; accelerated rendering is required");
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser> /* browser */,
                            PaintElementType type,
                            // Always copy the full frame; damage does not change capture work.
                            const RectList& /* dirty_rects */,
                            const CefAcceleratedPaintInfo& info) override
    {
        // Native control popup surfaces are outside this headless graphics source.
        if (type == PET_POPUP || state_->close_requested || state_->phase == phase_e::failed) {
            return;
        }
        try {
            if (type != PET_VIEW) {
                throw std::runtime_error("Unknown CEF paint element");
            }
            if (capture_.capture(info, state_->close_requested)) {
                state_->phase = phase_e::ready;
            }
        } catch (const std::exception& failure) {
            state_->fail(failure.what());
        }
    }
};
} // namespace

struct session_s::impl_s
{
    std::shared_ptr<shared_state_s> state = std::make_shared<shared_state_s>();
    CefRefPtr<client_s>             client;
};

session_s::session_s(gpu::device_s& device, options_s options)
    : impl_(std::make_unique<impl_s>())
{
    if (!MIXIMUS_CEF_NATIVE_CAPTURE_READY) {
        throw std::runtime_error("CEF sessions require the verified native-handle completion SDK");
    }
    // CEF 152 has no historical 60 fps ceiling; its microsecond period must
    // remain nonzero. Actual delivery can be slower than the requested rate.
    if (options.url.empty() || options.dimensions.x < 1 || options.dimensions.y < 1 || options.dimensions.x > 8192 ||
        options.dimensions.y > 8192 || options.frame_rate < 1 || options.frame_rate > 1'000'000) {
        throw std::invalid_argument("Invalid CEF session options");
    }
    impl_->client = new client_s(device, std::move(options), impl_->state);
}

session_s::~session_s() = default;

detail::browser_session_s::~browser_session_s() { close_async(); }

void detail::browser_session_s::start_async()
{
    if (impl_->state->started.exchange(true)) {
        return;
    }
    if (!CefPostTask(TID_UI, new task_s([client = impl_->client] { client->start(); }))) {
        impl_->state->fail("Cannot dispatch CEF browser creation");
        impl_->state->mark_closed();
    }
}

void detail::browser_session_s::close_async()
{
    if (impl_->state->closed || impl_->state->close_requested.exchange(true)) {
        return;
    }
    impl_->state->commands->close();
    impl_->state->phase = phase_e::closing;
    if (!CefPostTask(TID_UI, new task_s([client = impl_->client] { client->close(); }))) {
        impl_->state->fail("Cannot dispatch CEF browser closure");
    }
}

bool detail::browser_session_s::closed() const noexcept { return impl_->state->closed; }

size_t detail::browser_session_s::texture_budget(const options_s& options)
{
    return capture_stream_s::texture_budget(options.dimensions);
}

bool detail::browser_session_s::resources_idle() const { return impl_->client->capture().idle(); }

bool session_s::context_ready() const noexcept { return impl_->state->commands->ready(); }

std::future<session_s::command_result_s>
session_s::request(std::string function_source, std::string json, std::chrono::milliseconds timeout)
{
    return impl_->state->commands->request(
        std::move(function_source), std::move(json), timeout, command_protocol::request_kind_e::custom);
}

std::future<session_s::command_result_s> session_s::set_program_time_handler(std::string function_source)
{
    return impl_->state->commands->request(
        std::move(function_source), "null", 5s, command_protocol::request_kind_e::timing_handler);
}

void session_s::send_program_time(core::frame_context_s time)
{
    if (impl_->state->commands->has_timing_handler()) {
        (void)impl_->state->commands->request({}, {}, 1s, command_protocol::request_kind_e::program_time, time);
    }
}

bool detail::browser_session_s::wait_closed(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(impl_->state->mutex);
    return impl_->state->changed.wait_for(lock, timeout, [&] { return closed(); });
}

void session_s::advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity)
{
    impl_->client->capture().advance_frames(pts, target_time, discontinuity);
}
bool                   session_s::submit_frame(utils::flicks pts) { return impl_->client->capture().submit_frame(pts); }
session_s::frame_ptr_t session_s::resolve_frame() { return impl_->client->capture().resolve_frame(); }
void                   session_s::release_prepared_frame() { impl_->client->capture().release_prepared_frame(); }
void                   session_s::reset_frames() { impl_->client->capture().reset_frames(); }

session_s::metrics_s session_s::metrics() const
{
    auto                   result   = impl_->client->capture().metrics();
    const auto             commands = impl_->state->commands->metrics();
    const std::scoped_lock lock(impl_->state->mutex);
    result.phase             = impl_->state->phase;
    result.error             = impl_->state->error;
    result.timing_rejections = commands.rejections;
    result.timing_error      = commands.error;
    return result;
}

} // namespace miximus::nodes::cef
