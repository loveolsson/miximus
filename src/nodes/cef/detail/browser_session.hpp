#pragma once
#include "nodes/cef/session.hpp"
namespace miximus::nodes::cef::detail {
// Lifecycle authority is held by the subsystem control worker. Consumers receive
// session_s, which exposes frame consumption, commands and status only.
class browser_session_s final : public session_s
{
  public:
    browser_session_s(gpu::device_s& device, options_s options, std::shared_ptr<media_input_runtime_s> inputs = {})
        : session_s(device, std::move(options), std::move(inputs))
    {
    }
    ~browser_session_s();
    browser_session_s(const browser_session_s&)            = delete;
    browser_session_s& operator=(const browser_session_s&) = delete;
    browser_session_s(browser_session_s&&)                 = delete;
    browser_session_s& operator=(browser_session_s&&)      = delete;
    void               start_async();
    void               close_async();
    bool               reload_async(bool ignore_cache);
    bool               closed() const noexcept;
    bool               wait_closed(std::chrono::milliseconds timeout) const;
    static size_t      texture_budget(const options_s& options);
    bool               resources_idle() const;
    using session_s::reset_frames;
};
} // namespace miximus::nodes::cef::detail
