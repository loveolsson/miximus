#pragma once

#include "command_messages.hpp"
#include "include/cef_browser.h"
#include "nodes/cef/session.hpp"

#include <atomic>
#include <map>
#include <mutex>

namespace miximus::nodes::cef::detail {

// Bounded command transport. Browser/context handles belong to CEF's UI thread;
// admission, cancellation and metric snapshots synchronize through this component.
class command_channel_s : public std::enable_shared_from_this<command_channel_s>
{
    struct pending_s
    {
        std::promise<session_s::command_result_s> promise;
        std::string                               generation;
        std::string                               context;
        CefRefPtr<CefFrame>                       frame;
        std::chrono::steady_clock::time_point     deadline;
        bool                                      settled{};
        command_protocol::request_kind_e          kind{command_protocol::request_kind_e::custom};
        std::optional<core::frame_context_s>      time;
    };
    std::map<std::string, pending_s> pending_requests;
    uint64_t                         next_request{};
    uint64_t                         navigation{};
    std::string                      context;
    std::atomic_bool                 context_ready;
    bool                             timeout_check_scheduled{};
    std::atomic_bool                 timing_enabled;
    uint64_t                         timing_rejections{};
    std::string                      timing_error;
    mutable std::mutex               mutex;
    std::atomic_bool                 close_requested;
    CefRefPtr<CefBrowser>            browser_; // CEF UI thread only.

    void dispatch_command(const std::string& id, const std::string& source, const std::string& json);
    void receive_command_result(const CefRefPtr<CefListValue>& args);
    void schedule_timeout_check();
    void expire_command(const std::string& id, std::string reason);

  public:
    void attach(CefRefPtr<CefBrowser> browser) { browser_ = std::move(browser); }
    void detach() { browser_ = nullptr; }
    void close()
    {
        close_requested = true;
        cancel_commands("Browser is closing");
    }
    void cancel_commands(std::string_view reason);
    bool receive(const CefRefPtr<CefFrame>& frame, CefProcessId source, const CefRefPtr<CefProcessMessage>& message);
    bool ready() const noexcept { return context_ready; }
    bool has_timing_handler() const noexcept { return timing_enabled; }
    struct metrics_s
    {
        uint64_t    rejections;
        std::string error;
    };
    metrics_s metrics() const
    {
        const std::scoped_lock lock(mutex);
        return {timing_rejections, timing_error};
    }
    std::future<session_s::command_result_s> request(std::string                          function_source,
                                                     std::string                          json,
                                                     std::chrono::milliseconds            timeout,
                                                     command_protocol::request_kind_e     kind,
                                                     std::optional<core::frame_context_s> time = {});
};
} // namespace miximus::nodes::cef::detail
