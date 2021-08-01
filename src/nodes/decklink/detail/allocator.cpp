#include "allocator.hpp"

#include <cassert>

namespace miximus::nodes::decklink::detail {

allocator_s::allocator_s(const std::shared_ptr<gpu::context_s>& ctx, gpu::transfer::transfer_i::direction_e dir)
    : transfer_type_(transfer_i::get_prefered_type())
    , ctx_(ctx)
    , direction_(dir)
{
}

HRESULT allocator_s::AllocateBuffer(uint32_t bufferSize, void** allocatedBuffer)
{
    auto lock = ctx_->get_lock();

    last_allocation_size_ = bufferSize;
    ctx_->make_current();

    if (!free_transfers_.empty()) {
        auto tr = std::move(free_transfers_.front());
        free_transfers_.pop_front();

        if (tr->size() == bufferSize) {
            *allocatedBuffer = tr->ptr();
            assert(*allocatedBuffer != nullptr);

            allocated_transfers_.emplace(*allocatedBuffer, std::move(tr));
            gpu::context_s::rewind_current();
            return S_OK;
        }
    }

    auto tr          = transfer_i::create_transfer(transfer_type_, bufferSize, direction_);
    *allocatedBuffer = tr->ptr();
    assert(*allocatedBuffer != nullptr);

    allocated_transfers_.emplace(*allocatedBuffer, std::move(tr));

    gpu::context_s::rewind_current();
    return S_OK;
}

HRESULT allocator_s::ReleaseBuffer(void* buffer)
{
    auto lock = ctx_->get_lock();

    auto has_current = gpu::context_s::has_current();

    if (!has_current) {
        ctx_->make_current();
    }

    auto it = allocated_transfers_.find(buffer);
    if (it != allocated_transfers_.end()) {
        if (active_ && free_transfers_.size() < 2 && it->second->size() == last_allocation_size_) {
            free_transfers_.emplace_back(std::move(it->second));
        }

        allocated_transfers_.erase(it);
    } else {
        throw std::runtime_error("DeckLink allocator trying to release unknown frame");
    }

    if (!has_current) {
        gpu::context_s::rewind_current();
    }

    return S_OK;
}

HRESULT allocator_s::Commit(void)
{
    auto lock = ctx_->get_lock();
    active_   = true;
    return S_OK;
}

HRESULT allocator_s::Decommit(void)
{
    auto lock = ctx_->get_lock();
    free_transfers_.clear();
    active_ = false;
    return S_OK;
}

gpu::transfer::transfer_i* allocator_s::get_transfer(void* ptr)
{
    // Does not lock since it is called from locked contexts

    auto it = allocated_transfers_.find(ptr);
    if (it != allocated_transfers_.end()) {
        return it->second.get();
    }

    for (auto& t : free_transfers_) {
        assert(t->ptr() != ptr);
    }

    return nullptr;
}

bool allocator_s::register_texture(const gpu::texture_s& texture)
{
    return transfer_i::register_texture(transfer_type_, texture);
}

bool allocator_s::unregister_texture(const gpu::texture_s& texture)
{
    return transfer_i::unregister_texture(transfer_type_, texture);
}

bool allocator_s::begin_texture_use(const gpu::texture_s& texture)
{
    return transfer_i::begin_texture_use(transfer_type_, texture);
}

bool allocator_s::end_texture_use(const gpu::texture_s& texture)
{
    return transfer_i::end_texture_use(transfer_type_, texture);
}

size_t allocator_s::destroy_free_transfers()
{
    auto lock = ctx_->get_lock();

    free_transfers_.clear();

    return allocated_transfers_.size();
}

} // namespace miximus::nodes::decklink::detail
