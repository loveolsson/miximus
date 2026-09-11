#include "buffer.hpp"

#include "detail/device.hpp"

#include <stdexcept>
#include <utility>

namespace miximus::gpu::detail {

void buffer_state_s::flush_host_writes()
{
    if (!host_dirty.exchange(false)) {
        return;
    }

    try {
        check(vmaFlushAllocation(owner->allocator, allocation, 0, VK_WHOLE_SIZE), "flush host writes");
    } catch (...) {
        host_dirty = true;
        throw;
    }
}

buffer_state_s::~buffer_state_s()
{
    if (buffer == nullptr) {
        return;
    }

    if (external_buffer) {
        owner->retire(last_use_timeline_value.load(),
                      [device  = owner->device,
                       destroy = owner->vk.vkDestroyBuffer,
                       free    = owner->vk.vkFreeMemory,
                       handle  = buffer,
                       memory  = external_memory] {
                          destroy(device, handle, nullptr);
                          if (memory) {
                              free(device, memory, nullptr);
                          }
                      });
        return;
    }

    owner->retire(last_use_timeline_value.load(), [allocator = owner->allocator, handle = buffer, memory = allocation] {
        vmaDestroyBuffer(allocator, handle, memory);
    });
}
} // namespace miximus::gpu::detail

namespace miximus::gpu {

using detail::check;

buffer_s::buffer_s(std::shared_ptr<detail::buffer_state_s> state)
    : state_(std::move(state))
{
}

allocation_info_s buffer_s::allocation_info() const
{
    if (!state_) {
        throw std::logic_error("empty buffer");
    }

    return state_->info;
}

size_t buffer_s::size() const
{
    if (!state_) {
        throw std::logic_error("empty buffer");
    }

    return state_->bytes;
}

std::span<std::byte> buffer_s::writable_bytes()
{
    if (!state_) {
        throw std::logic_error("empty buffer");
    }

    state_->check_host_access();
    if ((state_->mapped == nullptr) || state_->access == host_access_e::readback) {
        throw std::logic_error("buffer is not CPU-writable");
    }

    // Preserve unflushed CPU edits; invalidation is only safe before a new write cycle.
    if (state_->access == host_access_e::read_write && !state_->host_dirty.load()) {
        check(vmaInvalidateAllocation(state_->owner->allocator, state_->allocation, 0, VK_WHOLE_SIZE),
              "invalidate mapped memory");
    }

    state_->host_dirty = true;
    return {static_cast<std::byte*>(state_->mapped), state_->bytes};
}

std::span<const std::byte> buffer_s::readable_bytes() const
{
    if (!state_) {
        throw std::logic_error("empty buffer");
    }

    state_->check_host_access();
    if ((state_->mapped == nullptr) || state_->access == host_access_e::sequential_write) {
        throw std::logic_error("buffer is not CPU-readable");
    }

    // Host access was checked above, so GPU writes have retired before invalidation.
    if (!state_->host_dirty.load()) {
        check(vmaInvalidateAllocation(state_->owner->allocator, state_->allocation, 0, VK_WHOLE_SIZE),
              "invalidate readback");
    }

    return {static_cast<const std::byte*>(state_->mapped), state_->bytes};
}

bool buffer_s::idle() const
{
    return !state_ ||
           (state_->recording_uses.load() == 0 && state_->last_use_timeline_value.load() <= state_->owner->completed());
}

} // namespace miximus::gpu
