#include "completion.hpp"

#include "detail/device.hpp"
#include "detail/recording.hpp"
#include "detail/resource.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace miximus::gpu {

using detail::check;

completion_s::completion_s(std::shared_ptr<detail::device_state_s>     state,
                           std::shared_ptr<detail::submission_state_s> submission)
    : state_(std::move(state))
    , submission_(std::move(submission))
{
}

bool completion_s::ready() const
{
    if (!submission_) {
        return true;
    }
    if (submission_->failed.load(std::memory_order_acquire)) {
        throw std::runtime_error("GPU submission failed");
    }
    const auto value = submission_->value.load(std::memory_order_acquire);
    return value != 0 && state_->completed() >= value;
}

bool completion_s::submitted() const
{
    if (!submission_) {
        return true;
    }
    if (submission_->failed.load(std::memory_order_acquire)) {
        throw std::runtime_error("GPU submission failed");
    }
    return submission_->value.load(std::memory_order_acquire) != 0;
}

wait_result_e completion_s::wait_submitted(std::chrono::milliseconds timeout, const std::stop_token& stop) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (submission_) {
        if (stop.stop_requested()) {
            return wait_result_e::cancelled;
        }
        if (submission_->failed.load()) {
            throw std::runtime_error("GPU submission failed");
        }
        if (submission_->value.load(std::memory_order_acquire) != 0) {
            return wait_result_e::ready;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return wait_result_e::timeout;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return wait_result_e::ready;
}

wait_result_e completion_s::wait(std::chrono::milliseconds timeout, const std::stop_token& stop) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (stop.stop_requested()) {
            return wait_result_e::cancelled;
        }
        if (ready()) {
            return wait_result_e::ready;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) {
            return wait_result_e::timeout;
        }
        const auto value = submission_->value.load(std::memory_order_acquire);
        if (value != 0) {
            VkSemaphoreWaitInfo info{};
            info.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            info.semaphoreCount = 1;
            info.pSemaphores    = &state_->submissions.timeline;
            info.pValues        = &value;
            const auto duration = std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining),
                                           std::chrono::nanoseconds(std::chrono::milliseconds(5)));
            const auto result =
                state_->vk.vkWaitSemaphores(state_->device, &info, static_cast<uint64_t>(duration.count()));
            if (result != VK_TIMEOUT) {
                check(result, "timeline wait");
            }
        }
        // The submission worker also releases recording pins before publishing
        // completion, so a woken host reader can safely access mapped memory.
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}
} // namespace miximus::gpu
