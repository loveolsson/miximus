#include "browser_session.hpp"

#include "command_protocol.hpp"
#include "gpu/detail/dma_buf_copy.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_parser.h"
#include "include/cef_process_message.h"
#include "include/cef_task.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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
    struct pending_s
    {
        std::promise<browser_session_s::command_result_s> promise;
        std::string                                       generation;
        std::string                                       context;
        CefRefPtr<CefFrame>                               frame;
        std::chrono::steady_clock::time_point             deadline;
        bool                                              settled{};
        command_protocol::request_kind_e                  kind{command_protocol::request_kind_e::custom};
        std::optional<core::frame_context_s>              time;
    };
    std::map<std::string, pending_s> pending;
    uint64_t                         next_request{};
    uint64_t                         navigation{};
    std::string                      context;
    std::atomic_bool                 context_ready{};
    bool                             timeout_check_scheduled{};
    std::atomic_bool                 timing_enabled{};
    uint64_t                         timing_rejections{};
    std::string                      timing_error;
    std::atomic<phase_e>             phase{phase_e::starting};
    std::atomic_bool                 started{};
    std::atomic_bool                 close_requested{};
    std::atomic_bool                 closed{};
    std::atomic_uint64_t             received{};
    std::atomic_uint64_t             copied{};
    std::atomic_uint64_t             dropped{};
    mutable std::mutex               mutex;
    mutable std::condition_variable  changed;
    std::string                      error;
    capture_timing_s                 capture_timing;
    capture_timing_s                 completion_wait_timing;
    frame_queue_t                    frames{
                           {.capacity = 4, .playout_delay_frames = 1}
    };

    void cancel_commands(std::string_view reason)
    {
        const std::lock_guard lock(mutex);
        context_ready  = false;
        timing_enabled = false;
        context.clear();
        ++navigation;
        for (auto& [id, request] : pending)
            if (!request.settled)
                request.promise.set_value({.json = {}, .error = std::string(reason)});
        pending.clear();
    }

    void expire_command(const std::string& id, std::string reason)
    {
        const std::lock_guard lock(mutex);
        const auto            found = pending.find(id);
        if (found == pending.end() || found->second.settled)
            return;
        if (found->second.frame) {
            auto message = CefProcessMessage::Create(command_protocol::CANCEL);
            message->GetArgumentList()->SetString(0, id);
            message->GetArgumentList()->SetString(1, found->second.context);
            found->second.frame->SendProcessMessage(PID_RENDERER, message);
        }
        if (found->second.kind == command_protocol::request_kind_e::program_time) {
            ++timing_rejections;
            timing_error = reason;
        }
        found->second.promise.set_value({.json = {}, .error = std::move(reason)});
        // Retain the in-flight slot until the renderer acknowledges cancellation
        // or returns its result. A hung renderer cannot grow its IPC backlog.
        found->second.settled = true;
        if (!found->second.frame)
            pending.erase(found);
    }

    void fail(std::string message)
    {
        std::lock_guard lock(mutex);
        if (error.empty())
            error = std::move(message);
        phase = phase_e::failed;
    }

    void mark_closed()
    {
        cancel_commands("Browser closed");
        {
            std::lock_guard lock(mutex);
            closed = true;
            phase  = error.empty() ? phase_e::closed : phase_e::failed;
        }
        changed.notify_all();
    }
};

// At most one timeout task per session, regardless of how many fast commands
// finish before their deadlines. The task owns no browser or GPU resources.
void schedule_timeout_check(const std::shared_ptr<shared_state_s>& state)
{
    if (!CefPostDelayedTask(TID_UI,
                            new task_s([weak = std::weak_ptr(state)] {
                                const auto state = weak.lock();
                                if (!state)
                                    return;
                                std::vector<std::string> expired;
                                {
                                    const std::lock_guard lock(state->mutex);
                                    if (std::ranges::none_of(state->pending,
                                                             [](const auto& item) { return !item.second.settled; })) {
                                        state->timeout_check_scheduled = false;
                                        return;
                                    }
                                    const auto now = std::chrono::steady_clock::now();
                                    for (const auto& [id, request] : state->pending)
                                        if (!request.settled && now >= request.deadline)
                                            expired.push_back(id);
                                }
                                for (const auto& id : expired)
                                    state->expire_command(id, "Browser command timed out");
                                schedule_timeout_check(state);
                            }),
                            10)) {
        state->cancel_commands("Cannot schedule browser command timeout");
        const std::lock_guard lock(state->mutex);
        state->timeout_check_scheduled = false;
    }
}

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

    CefRefPtr<CefJSDialogHandler>    GetJSDialogHandler() override { return this; }
    CefRefPtr<CefDialogHandler>      GetDialogHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

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
        state_->cancel_commands("Browser is closing");
        if (browser_)
            browser_->GetHost()->CloseBrowser(true);
        else if (!creation_pending_)
            state_->mark_closed();
    }

    void GetViewRect(CefRefPtr<CefBrowser> /* browser */, CefRect& rect) override
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

    void OnBeforeClose(CefRefPtr<CefBrowser> /* browser */) override
    {
        browser_ = nullptr;
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
            state_->cancel_commands("Browser navigated");
            ++epoch_;
            previous_timestamp_.reset();
            state_->phase = phase_e::loading;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser> /* browser */,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode           code,
                     const CefString&    text,
                     const CefString& /* failed_url */) override
    {
        if (frame->IsMain() && code != ERR_ABORTED && !state_->close_requested)
            state_->fail(std::format("CEF load error {}: {}", static_cast<int>(code), text.ToString()));
    }

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> /* browser */,
                                   TerminationStatus /* status */,
                                   int              code,
                                   const CefString& text) override
    {
        state_->cancel_commands("Browser renderer terminated");
        state_->fail(std::format("CEF renderer terminated {}: {}", code, text.ToString()));
    }

    void dispatch_command(const std::string& id, const std::string& source, const std::string& json)
    {
        const std::lock_guard lock(state_->mutex);
        const auto            found = state_->pending.find(id);
        if (found == state_->pending.end())
            return;
        if (!browser_ || !state_->context_ready || state_->close_requested) {
            found->second.promise.set_value({.json = {}, .error = "Browser JavaScript context is unavailable"});
            state_->pending.erase(found);
            return;
        }
        auto& request      = found->second;
        request.frame      = browser_->GetMainFrame();
        request.generation = std::to_string(state_->navigation);
        request.context    = state_->context;
        auto message       = CefProcessMessage::Create(command_protocol::REQUEST);
        auto args          = message->GetArgumentList();
        args->SetString(0, id);
        args->SetString(1, request.generation);
        args->SetString(2, request.context);
        args->SetString(3, source);
        if (request.time) {
            const auto& time  = *request.time;
            auto        value = CefDictionaryValue::Create();
            value->SetString("epoch", std::to_string(time.epoch));
            value->SetString("frameNumber", std::to_string(time.frame_number));
            value->SetString("pts", std::to_string(time.program_pts.count()));
            value->SetString("duration", std::to_string(time.frame_duration.count()));
            value->SetString("timebase", std::to_string(utils::flicks::period::den));
            value->SetDouble("milliseconds", std::chrono::duration<double, std::milli>(time.program_pts).count());
            value->SetBool("discontinuity", time.discontinuity);
            auto payload = CefValue::Create();
            payload->SetDictionary(value);
            args->SetString(4, CefWriteJSON(payload, JSON_WRITER_DEFAULT));
        } else {
            args->SetString(4, json);
        }
        args->SetInt(5, static_cast<int>(request.kind));
        request.frame->SendProcessMessage(PID_RENDERER, message);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /* browser */,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        if (source != PID_RENDERER || !frame->IsMain() || !browser_ ||
            frame->GetIdentifier() != browser_->GetMainFrame()->GetIdentifier())
            return false;
        const auto            name = message->GetName();
        const auto            args = message->GetArgumentList();
        const std::lock_guard lock(state_->mutex);
        if (name == command_protocol::CONTEXT_READY && args->GetSize() == 1) {
            state_->context       = args->GetString(0).ToString();
            state_->context_ready = !state_->close_requested;
            return true;
        }
        if (name == command_protocol::CONTEXT_RELEASED && args->GetSize() == 1) {
            if (state_->context == args->GetString(0).ToString()) {
                state_->context_ready  = false;
                state_->timing_enabled = false;
                state_->context.clear();
                for (auto& [id, request] : state_->pending)
                    if (!request.settled)
                        request.promise.set_value({.json = {}, .error = "JavaScript context was released"});
                state_->pending.clear();
            }
            return true;
        }
        if (name != command_protocol::RESULT)
            return false;
        if (args->GetSize() != 5)
            return true;
        const auto found = state_->pending.find(args->GetString(0).ToString());
        if (found == state_->pending.end() || found->second.generation != args->GetString(1).ToString() ||
            found->second.context != args->GetString(2).ToString())
            return true;
        if (found->second.settled) {
            state_->pending.erase(found);
            return true;
        }
        browser_session_s::command_result_s result;
        auto                                payload = args->GetString(4).ToString();
        if (payload.size() > command_protocol::MAX_JSON_BYTES)
            result.error = "JavaScript result exceeds the JSON response limit";
        else if (!args->GetBool(3))
            result.error = std::move(payload);
        else if (!CefParseJSON(payload, JSON_PARSER_RFC))
            result.error = "JavaScript result is not valid JSON";
        else
            result.json = std::move(payload);
        if (found->second.kind == command_protocol::request_kind_e::timing_handler && result.error.empty()) {
            state_->timing_enabled = true;
            state_->timing_error.clear();
        }
        if (found->second.kind == command_protocol::request_kind_e::program_time && !result.error.empty()) {
            ++state_->timing_rejections;
            state_->timing_error = result.error;
        }
        found->second.promise.set_value(std::move(result));
        state_->pending.erase(found);
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser> /* browser */,
                 PaintElementType type,
                 const RectList& /* dirty_rects */,
                 const void* /* buffer */,
                 int /* width */,
                 int /* height */) override
    {
        if (type == PET_POPUP)
            return;
        state_->fail("CEF delivered software paint; accelerated rendering is required");
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser> /* browser */,
                            PaintElementType type,
                            // Always copy the full frame; damage does not change capture work.
                            const RectList& /* dirty_rects */,
                            const CefAcceleratedPaintInfo& info) override
    {
        // Native control popup surfaces are outside this headless graphics source.
        if (type == PET_POPUP || state_->close_requested || state_->phase == phase_e::failed)
            return;
        const auto capture_started = std::chrono::steady_clock::now();
        const auto arrival         = utils::flicks_now();
        const auto sequence        = ++state_->received;
        try {
            if (type != PET_VIEW)
                throw std::runtime_error("Unknown CEF paint element");
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
            const auto wait_started = std::chrono::steady_clock::now();
            while (completion.wait(1s) != gpu::wait_result_e::ready) {
            }
            const auto wait_finished = std::chrono::steady_clock::now();
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
            {
                const auto            finished = std::chrono::steady_clock::now();
                const std::lock_guard lock(state_->mutex);
                state_->capture_timing.add(finished - capture_started);
                state_->completion_wait_timing.add(wait_finished - wait_started);
                ++state_->copied;
            }
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
    impl_->state->cancel_commands("Browser is closing");
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

bool browser_session_s::context_ready() const noexcept { return impl_->state->context_ready; }

std::future<browser_session_s::command_result_s>
browser_session_s::request(std::string function_source, std::string json, std::chrono::milliseconds timeout)
{
    return request_impl(std::move(function_source), std::move(json), timeout, command_protocol::request_kind_e::custom);
}

std::future<browser_session_s::command_result_s>
browser_session_s::set_program_time_handler(std::string function_source)
{
    return request_impl(std::move(function_source), "null", 5s, command_protocol::request_kind_e::timing_handler);
}

void browser_session_s::send_program_time(core::frame_context_s time)
{
    if (impl_->state->timing_enabled)
        (void)request_impl({}, {}, 1s, command_protocol::request_kind_e::program_time, time);
}

std::future<browser_session_s::command_result_s>
browser_session_s::request_impl(std::string                          function_source,
                                std::string                          json,
                                std::chrono::milliseconds            timeout,
                                command_protocol::request_kind_e     kind,
                                std::optional<core::frame_context_s> time)
{
    auto                      state = impl_->state;
    shared_state_s::pending_s pending;
    auto                      result = pending.promise.get_future();
    if (function_source.size() > command_protocol::MAX_SOURCE_BYTES || json.size() > command_protocol::MAX_JSON_BYTES ||
        timeout <= 0ms || timeout > 30s || (!time && !CefParseJSON(json, JSON_PARSER_RFC))) {
        pending.promise.set_value({.json = {}, .error = "Invalid command payload or timeout"});
        return result;
    }
    std::string id;
    bool        start_timer{};
    pending.deadline = std::chrono::steady_clock::now() + timeout;
    pending.kind     = kind;
    pending.time     = time;
    {
        const std::lock_guard lock(state->mutex);
        if (state->close_requested || !state->context_ready || state->pending.size() >= command_protocol::MAX_PENDING) {
            if (kind == command_protocol::request_kind_e::program_time) {
                ++state->timing_rejections;
                state->timing_error = "Program-time dispatch capacity exhausted or context unavailable";
            }
            pending.promise.set_value(
                {.json = {}, .error = "Browser context unavailable or command capacity exhausted"});
            return result;
        }
        id = std::to_string(++state->next_request);
        state->pending.emplace(id, std::move(pending));
        start_timer = !std::exchange(state->timeout_check_scheduled, true);
    }
    if (start_timer)
        schedule_timeout_check(state);
    if (!CefPostTask(
            TID_UI,
            new task_s([client = impl_->client, id, source = std::move(function_source), json = std::move(json)] {
                client->dispatch_command(id, source, json);
            }))) {
        state->expire_command(id, "Cannot dispatch browser command");
        return result;
    }
    return result;
}

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
    return {.phase             = state.phase.load(),
            .error             = state.error,
            .received          = state.received.load(),
            .copied            = state.copied.load(),
            .dropped           = state.dropped.load(),
            .timing_rejections = state.timing_rejections,
            .timing_error      = state.timing_error,
            .capture           = state.capture_timing.snapshot(),
            .completion_wait   = state.completion_wait_timing.snapshot(),
            .source_queue      = state.frames.metrics()};
}

} // namespace miximus::nodes::cef::detail
