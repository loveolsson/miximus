#pragma once
#include "gpu/context.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace miximus::nodes::decklink::detail {

class allocator_s : public IDeckLinkMemoryAllocator
{
    using transfer_i = gpu::transfer::transfer_i;

    using transfer_ptr_t   = std::unique_ptr<transfer_i>;
    using transfer_map_t   = std::map<void*, transfer_ptr_t>;
    using transfer_queue_t = std::deque<transfer_ptr_t>;

    const transfer_i::type_e        transfer_type_;
    const transfer_i::direction_e   direction_;
    std::shared_ptr<gpu::context_s> ctx_;

    transfer_map_t   allocated_transfers_;
    transfer_queue_t free_transfers_;
    bool             active_{false};
    size_t           last_allocation_size_{};

    std::atomic_ulong ref_count_{1};

    static inline std::atomic_size_t allocations_g{0};

  public:
    allocator_s(std::shared_ptr<gpu::context_s> ctx, gpu::transfer::transfer_i::direction_e dir);
    ~allocator_s();

    HRESULT AllocateBuffer(uint32_t bufferSize, void** allocatedBuffer) final;
    HRESULT ReleaseBuffer(void* buffer) final;
    HRESULT Commit(void) final;
    HRESULT Decommit(void) final;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE   AddRef(void) final { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE   Release(void) final
    {
        ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    transfer_i* get_transfer(void* ptr);

    bool register_texture(gpu::texture_s* texture);
    bool unregister_texture(gpu::texture_s* texture);
    bool begin_texture_use(gpu::texture_s* texture);
    bool end_texture_use(gpu::texture_s* texture);

    size_t destroy_free_transfers();
};

} // namespace miximus::nodes::decklink::detail