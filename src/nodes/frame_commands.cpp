#include "frame_commands.hpp"

#include "gpu/device.hpp"
#include "gpu/recording.hpp"

#include <stdexcept>
#include <utility>

namespace miximus::nodes {
frame_commands_s::frame_commands_s(gpu::device_s* device)
    : device_(device)
{
}

frame_commands_s::~frame_commands_s() = default;

gpu::recording_s& frame_commands_s::commands()
{
    if (device_ == nullptr) {
        throw std::logic_error("GPU commands require a device");
    }
    if (!recording_) {
        recording_ = device_->try_record();
        if (!recording_) {
            throw gpu::recording_unavailable_s{};
        }
    }

    return *recording_;
}

gpu::completion_s frame_commands_s::submit_gpu()
{
    if (recording_) {
        last_submission_ = recording_->submit();
        recording_.reset();
    }

    return last_submission_;
}

void frame_commands_s::defer_output(std::function<void(gpu::completion_s)> publish)
{
    pending_outputs_.emplace_back(std::move(publish));
}

void frame_commands_s::commit()
{
    if (!pending_outputs_.empty()) {
        auto& recording = commands();
        for (auto& publish : pending_outputs_) {
            recording.on_submitted(std::move(publish));
        }
        pending_outputs_.clear();
    }
    submit_gpu();
}

void frame_commands_s::abort() noexcept
{
    recording_.reset();
    pending_outputs_.clear();
}
} // namespace miximus::nodes
