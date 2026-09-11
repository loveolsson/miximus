#pragma once

#include "resource_types.hpp"

#include <chrono>
#include <memory>
#include <stop_token>

namespace miximus::gpu {

class completion_s
{
    std::shared_ptr<detail::device_state_s> state_;

    std::shared_ptr<detail::submission_state_s> submission_;
    completion_s(std::shared_ptr<detail::device_state_s> state, std::shared_ptr<detail::submission_state_s> submission);
    friend class recording_s;
    friend struct detail::recording_state_s;
    friend struct detail::presenter_state_s;
    friend class transfer::detail::cuda_staging_s;

  public:
    completion_s() = default;

    bool          ready() const;
    bool          submitted() const;
    wait_result_e wait_submitted(std::chrono::milliseconds timeout, const std::stop_token& stop = {}) const;
    wait_result_e wait(std::chrono::milliseconds timeout, const std::stop_token& stop = {}) const;
    explicit      operator bool() const noexcept { return submission_ != nullptr; }
};

} // namespace miximus::gpu
