#include "allocator.hpp"

#include "gpu/context.hpp"
#include "logger/logger.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <utility>

namespace miximus::nodes::decklink::detail {

constexpr size_t MAX_ALLOCATIONS = 4;

// Defined here because it references allocator_s::return_buffer.
ULONG video_buffer_s::Release()
{
    const ULONG count = --ref_count_;
    if (count == 0) {
        allocator_->return_buffer(this);
    }
    return count;
}

allocator_s::allocator_s(std::shared_ptr<gpu::context_s> ctx, gpu::transfer::transfer_i::direction_e dir)
    : transfer_type_(transfer_i::get_prefered_type())
    , direction_(dir)
    , ctx_(std::move(ctx))
{
}

allocator_s::~allocator_s()
{
    assert(free_buffers_.empty());
    assert(allocated_buffers_.empty());
}

HRESULT allocator_s::AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer)
{
    // No GL context needed here: transfer_i construction only does aligned_alloc.
    // make_current is deliberately omitted — DeckLink may call this from its own
    // driver thread (including for auto-conversion target frames), and acquiring
    // the GL context here would risk deadlocking against the render thread.
    auto lock = ctx_->get_lock();

    if (buffer_size_ == 0) {
        return E_OUTOFMEMORY;
    }

    if (!free_buffers_.empty()) {
        auto& front = free_buffers_.front();
        if (front->buffer_size() == buffer_size_) {
            auto* raw = front.get();
            raw->reset_ref_count();
            allocated_buffers_.emplace(raw, std::move(front));
            free_buffers_.pop_front();
            *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(raw);
            return S_OK;
        }
        // Wrong size — discard and fall through to allocate a new one.
        allocations_g--;
        free_buffers_.pop_front();
    }

    if (allocated_buffers_.size() >= MAX_ALLOCATIONS) {
        return E_OUTOFMEMORY;
    }

    getlog("decklink")->debug("Allocating transfer, current count {}", ++allocations_g);
    auto  transfer = transfer_i::create_transfer(transfer_type_, buffer_size_, direction_);
    auto  buffer   = std::make_unique<video_buffer_s>(this, std::move(transfer));
    auto* raw      = buffer.get();
    allocated_buffers_.emplace(raw, std::move(buffer));

    *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(raw);
    return S_OK;
}

void allocator_s::return_buffer(video_buffer_s* buffer)
{
    const gpu::context_scope_s context_scope(*ctx_, gpu::context_lock_e::lock);

    auto it = allocated_buffers_.find(buffer);
    if (it != allocated_buffers_.end()) {
        if (it->second->buffer_size() == buffer_size_) {
            free_buffers_.emplace_back(std::move(it->second));
        } else {
            getlog("decklink")->debug("Releasing transfer, current count {}", --allocations_g);
            // unique_ptr destroyed at erase below.
        }
        allocated_buffers_.erase(it);
    } else {
        throw std::runtime_error("DeckLink allocator trying to release unknown buffer");
    }
}

gpu::transfer::transfer_i* allocator_s::get_transfer(IDeckLinkVideoBuffer* buffer)
{
    return static_cast<video_buffer_s*>(buffer) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        ->get_transfer();
}

bool allocator_s::register_texture(gpu::texture_s* texture)
{
    return transfer_i::register_texture(transfer_type_, texture);
}

bool allocator_s::unregister_texture(gpu::texture_s* texture)
{
    return transfer_i::unregister_texture(transfer_type_, texture);
}

bool allocator_s::begin_texture_use(gpu::texture_s* texture)
{
    return transfer_i::begin_texture_use(transfer_type_, texture);
}

bool allocator_s::end_texture_use(gpu::texture_s* texture)
{
    return transfer_i::end_texture_use(transfer_type_, texture);
}

size_t allocator_s::destroy_free_transfers()
{
    allocations_g -= free_buffers_.size();
    free_buffers_.clear();

    getlog("decklink")->debug("Destroying free transfers, current count {}", allocations_g.load());

    return allocated_buffers_.size();
}

} // namespace miximus::nodes::decklink::detail
