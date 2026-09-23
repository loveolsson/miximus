#include "frame_pool.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace miximus::nodes::cef::detail {

struct frame_pool_s::state_s
{
    struct slot_s
    {
        gpu::texture_s texture;
        bool           leased{};
    };

    std::mutex          mutex;
    std::vector<slot_s> slots;

    state_s(gpu::device_s& device, gpu::vec2i_t dimensions, size_t capacity, size_t memory_budget_bytes)
    {
        if (dimensions.x <= 0 || dimensions.y <= 0 || capacity == 0) {
            throw std::invalid_argument("CEF frame pool dimensions and capacity must be positive");
        }
        constexpr auto format   = gpu::format_e::rgba_unorm16;
        constexpr auto sampling = gpu::sampling_e::linear;
        const auto     bytes    = gpu::texture_s::estimate_storage_byte_size(dimensions, format, sampling);
        if (bytes == 0 || capacity > memory_budget_bytes / bytes) {
            throw std::invalid_argument("CEF frame pool exceeds its texture memory budget");
        }

        slots.reserve(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            slots.push_back(
                {.texture = gpu::texture_s(device, dimensions, format, gpu::channel_order_e::rgba, sampling)});
        }
    }
};

frame_pool_s::frame_s::~frame_s()
{
    if (!state_) {
        return;
    }
    const std::scoped_lock lock(state_->mutex);
    state_->slots[index_].leased = false;
}

gpu::texture_s&       frame_pool_s::frame_s::texture() noexcept { return state_->slots[index_].texture; }
const gpu::texture_s& frame_pool_s::frame_s::texture() const noexcept { return state_->slots[index_].texture; }

frame_pool_s::frame_pool_s(gpu::device_s& device, gpu::vec2i_t dimensions, size_t capacity, size_t memory_budget_bytes)
    : state_(std::make_shared<state_s>(device, dimensions, capacity, memory_budget_bytes))
{
}

std::shared_ptr<frame_pool_s::frame_s> frame_pool_s::try_acquire()
{
    const std::scoped_lock lock(state_->mutex);
    for (size_t index = 0; index < state_->slots.size(); ++index) {
        auto& slot = state_->slots[index];
        if (!slot.leased && slot.texture.idle()) {
            // Allocate the lease before changing slot state; allocation failure
            // must not lose capacity. All access to the lease flag is serialized.
            auto frame    = std::shared_ptr<frame_s>(new frame_s);
            frame->state_ = state_;
            frame->index_ = index;
            slot.leased   = true;
            return frame;
        }
    }
    return {};
}

bool frame_pool_s::idle() const
{
    const std::scoped_lock lock(state_->mutex);
    return std::ranges::all_of(state_->slots, [](const auto& slot) { return !slot.leased && slot.texture.idle(); });
}

} // namespace miximus::nodes::cef::detail
