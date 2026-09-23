#include "browser_session.hpp"

#include "command_protocol.hpp"
#include "gpu/detail/dma_buf_copy.hpp"
#include "include/cef_browser.h"
#include "include/cef_client.h"
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
    };
    std::map<std::string, pending_s> pending;
    uint64_t                         next_request{};
    uint64_t                         navigation{};
    std::string                      context;
    std::atomic_bool                 context_ready{};
    bool                             timeout_check_scheduled{};
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
    frame_queue_t                    frames{
                           {.capacity = 4, .playout_delay_frames = 1}
    };

    void cancel_commands(std::string_view reason)
    {
        const std::lock_guard lock(mutex);
        context_ready = false;
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
        state_->cancel_commands("Browser is closing");
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
            state_->cancel_commands("Browser navigated");
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
        args->SetString(4, json);
        request.frame->SendProcessMessage(PID_RENDERER, message);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>,
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
                state_->context_ready = false;
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
        auto result = args->GetString(4).ToString();
        if (result.size() > command_protocol::MAX_JSON_BYTES)
            found->second.promise.set_value({.json = {}, .error = "JavaScript result exceeds the JSON response limit"});
        else if (!args->GetBool(3))
            found->second.promise.set_value({.json = {}, .error = std::move(result)});
        else if (!CefParseJSON(result, JSON_PARSER_RFC))
            found->second.promise.set_value({.json = {}, .error = "JavaScript result is not valid JSON"});
        else
            found->second.promise.set_value({.json = std::move(result), .error = {}});
        state_->pending.erase(found);
        return true;
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
    auto                      state = impl_->state;
    shared_state_s::pending_s pending;
    auto                      result = pending.promise.get_future();
    if (function_source.size() > command_protocol::MAX_SOURCE_BYTES || json.size() > command_protocol::MAX_JSON_BYTES ||
        timeout <= 0ms || timeout > 30s || !CefParseJSON(json, JSON_PARSER_RFC)) {
        pending.promise.set_value({.json = {}, .error = "Invalid command payload or timeout"});
        return result;
    }
    std::string id;
    bool        start_timer{};
    pending.deadline = std::chrono::steady_clock::now() + timeout;
    {
        const std::lock_guard lock(state->mutex);
        if (state->close_requested || !state->context_ready || state->pending.size() >= command_protocol::MAX_PENDING) {
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
    return {.phase        = state.phase.load(),
            .error        = state.error,
            .received     = state.received.load(),
            .copied       = state.copied.load(),
            .dropped      = state.dropped.load(),
            .source_queue = state.frames.metrics()};
}

} // namespace miximus::nodes::cef::detail
