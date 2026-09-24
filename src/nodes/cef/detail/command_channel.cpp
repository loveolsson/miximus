#include "command_channel.hpp"

#include "include/cef_parser.h"
#include "task.hpp"

#include <algorithm>

namespace miximus::nodes::cef::detail {

using namespace std::chrono_literals;

void command_channel_s::cancel_commands(std::string_view reason)
{
    const std::scoped_lock lock(mutex);
    context_ready  = false;
    timing_enabled = false;
    context.clear();
    ++navigation;
    for (auto& [id, request] : pending_requests) {
        if (!request.settled) {
            request.promise.set_value({.json = {}, .error = std::string(reason)});
        }
    }
    pending_requests.clear();
}

void command_channel_s::expire_command(const std::string& id, std::string reason)
{
    const std::scoped_lock lock(mutex);
    const auto             found = pending_requests.find(id);
    if (found == pending_requests.end() || found->second.settled) {
        return;
    }
    if (found->second.frame) {
        auto message = command_protocol::encode(command_protocol::cancel_s{.id = id, .context = found->second.context});
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
    if (!found->second.frame) {
        pending_requests.erase(found);
    }
}

// At most one timeout task per session, regardless of how many fast commands
// finish before their deadlines. The timer holds only a weak channel reference.
void command_channel_s::schedule_timeout_check()
{
    auto state = shared_from_this();
    if (!CefPostDelayedTask(TID_UI,
                            new task_s([weak = std::weak_ptr(state)] {
                                const auto state = weak.lock();
                                if (!state) {
                                    return;
                                }
                                std::vector<std::string> expired;
                                {
                                    const std::scoped_lock lock(state->mutex);
                                    if (std::ranges::none_of(state->pending_requests,
                                                             [](const auto& item) { return !item.second.settled; })) {
                                        state->timeout_check_scheduled = false;
                                        return;
                                    }
                                    const auto now = std::chrono::steady_clock::now();
                                    for (const auto& [id, request] : state->pending_requests) {
                                        if (!request.settled && now >= request.deadline) {
                                            expired.push_back(id);
                                        }
                                    }
                                }
                                for (const auto& id : expired) {
                                    state->expire_command(id, "Browser command timed out");
                                }
                                state->schedule_timeout_check();
                            }),
                            10)) {
        state->cancel_commands("Cannot schedule browser command timeout");
        const std::scoped_lock lock(state->mutex);
        state->timeout_check_scheduled = false;
    }
}

void command_channel_s::dispatch_command(const std::string& id, const std::string& source, const std::string& json)
{
    const std::scoped_lock lock(mutex);
    const auto             found = pending_requests.find(id);
    if (found == pending_requests.end()) {
        return;
    }
    if (!browser_ || !context_ready || close_requested) {
        found->second.promise.set_value({.json = {}, .error = "Browser JavaScript context is unavailable"});
        pending_requests.erase(found);
        return;
    }
    auto& request      = found->second;
    request.frame      = browser_->GetMainFrame();
    request.generation = std::to_string(navigation);
    request.context    = context;
    command_protocol::request_s message{.id         = id,
                                        .generation = request.generation,
                                        .context    = request.context,
                                        .source     = source,
                                        .json       = json,
                                        .kind       = request.kind};
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
        message.json = CefWriteJSON(payload, JSON_WRITER_DEFAULT);
    }
    request.frame->SendProcessMessage(PID_RENDERER, command_protocol::encode(message));
}

bool command_channel_s::receive(const CefRefPtr<CefFrame>&          frame,
                                CefProcessId                        source,
                                const CefRefPtr<CefProcessMessage>& message)
{
    if (source != PID_RENDERER || !frame->IsMain() || !browser_ ||
        frame->GetIdentifier() != browser_->GetMainFrame()->GetIdentifier()) {
        return false;
    }
    const auto             name = message->GetName();
    const auto             args = message->GetArgumentList();
    const std::scoped_lock lock(mutex);
    const auto             notification = command_protocol::decode_context(args);
    if (name == command_protocol::CONTEXT_READY && notification) {
        context       = notification->token;
        context_ready = !close_requested;
        return true;
    }
    if (name == command_protocol::CONTEXT_RELEASED && notification) {
        if (context == notification->token) {
            context_ready  = false;
            timing_enabled = false;
            context.clear();
            for (auto& [id, request] : pending_requests) {
                if (!request.settled) {
                    request.promise.set_value({.json = {}, .error = "JavaScript context was released"});
                }
            }
            pending_requests.clear();
        }
        return true;
    }
    if (name != command_protocol::RESULT) {
        return false;
    }
    receive_command_result(args);
    return true;
}

// Called on the CEF UI thread with mutex held.
void command_channel_s::receive_command_result(const CefRefPtr<CefListValue>& args)
{
    const auto message = command_protocol::decode_result(args);
    if (!message) {
        return;
    }
    const auto found = pending_requests.find(message->id);
    if (found == pending_requests.end() || found->second.generation != message->generation ||
        found->second.context != message->context) {
        return;
    }
    if (found->second.settled) {
        pending_requests.erase(found);
        return;
    }
    session_s::command_result_s result;
    auto                        payload = message->payload;
    if (payload.size() > command_protocol::MAX_JSON_BYTES) {
        result.error = "JavaScript result exceeds the JSON response limit";
    } else if (!message->success) {
        result.error = std::move(payload);
    } else if (!CefParseJSON(payload, JSON_PARSER_RFC)) {
        result.error = "JavaScript result is not valid JSON";
    } else {
        result.json = std::move(payload);
    }
    if (found->second.kind == command_protocol::request_kind_e::timing_handler && result.error.empty()) {
        timing_enabled = true;
        timing_error.clear();
    }
    if (found->second.kind == command_protocol::request_kind_e::program_time && !result.error.empty()) {
        ++timing_rejections;
        timing_error = result.error;
    }
    found->second.promise.set_value(std::move(result));
    pending_requests.erase(found);
}

std::future<session_s::command_result_s> command_channel_s::request(std::string                      function_source,
                                                                    std::string                      json,
                                                                    std::chrono::milliseconds        timeout,
                                                                    command_protocol::request_kind_e kind,
                                                                    std::optional<core::frame_context_s> time)
{
    auto      state = shared_from_this();
    pending_s pending;
    auto      result = pending.promise.get_future();
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
        const std::scoped_lock lock(state->mutex);
        if (state->close_requested || !state->context_ready ||
            state->pending_requests.size() >= command_protocol::MAX_PENDING) {
            if (kind == command_protocol::request_kind_e::program_time) {
                ++state->timing_rejections;
                state->timing_error = "Program-time dispatch capacity exhausted or context unavailable";
            }
            pending.promise.set_value(
                {.json = {}, .error = "Browser context unavailable or command capacity exhausted"});
            return result;
        }
        id = std::to_string(++state->next_request);
        state->pending_requests.emplace(id, std::move(pending));
        start_timer = !std::exchange(state->timeout_check_scheduled, true);
    }
    if (start_timer) {
        schedule_timeout_check();
    }
    if (!CefPostTask(TID_UI, new task_s([state, id, source = std::move(function_source), json = std::move(json)] {
                         state->dispatch_command(id, source, json);
                     }))) {
        state->expire_command(id, "Cannot dispatch browser command");
        return result;
    }
    return result;
}

} // namespace miximus::nodes::cef::detail
