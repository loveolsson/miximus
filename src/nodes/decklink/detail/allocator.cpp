#include "allocator.hpp"
#include "logger/logger.hpp"

#include <cassert>

namespace miximus::nodes::decklink::detail {

constexpr size_t       MAX_ALLOCATIONS = 4;
static std::atomic_int allocations_g{0};

allocator_s::allocator_s(std::shared_ptr<gpu::context_s> ctx, gpu::transfer::transfer_i::direction_e dir)
    : transfer_type_(transfer_i::get_prefered_type())
    , ctx_(std::move(ctx))
    , direction_(dir)
{
}

allocator_s::~allocator_s()
{
    assert(free_transfers_.empty());
    assert(allocated_transfers_.empty());
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

        allocations_g--;
    }

    if (allocated_transfers_.size() > MAX_ALLOCATIONS) {
        gpu::context_s::rewind_current();
        return E_OUTOFMEMORY;
    }

    getlog("gpu")->debug("Allocating transfer, currently {}", ++allocations_g);
    auto tr          = transfer_i::create_transfer(transfer_type_, bufferSize, direction_);
    *allocatedBuffer = tr->ptr();
    assert(*allocatedBuffer != nullptr);
    allocated_transfers_.emplace(*allocatedBuffer, std::move(tr));

    gpu::context_s::rewind_current();
    return S_OK;
}

HRESULT allocator_s::ReleaseBuffer(void* buffer)
{
    auto lock        = ctx_->get_lock();
    auto has_current = gpu::context_s::has_current();

    if (!has_current) {
        ctx_->make_current();
    }

    auto it = allocated_transfers_.find(buffer);
    if (it != allocated_transfers_.end()) {
        if (active_ && it->second->size() == last_allocation_size_) {
            free_transfers_.emplace_back(std::move(it->second));
        } else {
            getlog("gpu")->debug("Releasing transfer, {} left", --allocations_g);
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

HRESULT allocator_s::Commit()
{
    auto lock = ctx_->get_lock();
    active_   = true;
    return S_OK;
}

HRESULT allocator_s::Decommit()
{
    auto lock = ctx_->get_lock();

    allocations_g -= free_transfers_.size();
    free_transfers_.clear();

    getlog("gpu")->debug("Decommitting transfers, {} left", allocations_g);

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
    auto lock = ctx_->get_lock();

    free_transfers_.clear();

    return allocated_transfers_.size();
}

} // namespace miximus::nodes::decklink::detail
