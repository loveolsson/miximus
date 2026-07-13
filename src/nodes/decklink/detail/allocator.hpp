#pragma once
#include "gpu/context.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <memory>

namespace miximus::nodes::decklink::detail {

class allocator_s;

// Implements IDeckLinkVideoBuffer wrapping a GPU DMA transfer.
class video_buffer_s : public IDeckLinkVideoBuffer
{
    using transfer_i = gpu::transfer::transfer_i;

    allocator_s*                allocator_;
    std::unique_ptr<transfer_i> transfer_;
    std::atomic_ulong           ref_count_{1};

  public:
    video_buffer_s(allocator_s* allocator, std::unique_ptr<transfer_i> transfer)
        : allocator_(allocator)
        , transfer_(std::move(transfer))
    {
    }

    transfer_i* get_transfer() const { return transfer_.get(); }
    uint32_t    buffer_size() const { return static_cast<uint32_t>(transfer_->size()); }
    void        reset_ref_count() { ref_count_ = 1; }

    HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
    {
        *buffer = transfer_->ptr();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSize(uint64_t* size) override
    {
        *size = transfer_->size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags /*flags*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags /*flags*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* ppv) override
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() override; // defined in allocator.cpp
};

// Custom allocator for DeckLink GPU transfers. Implements IDeckLinkVideoBufferAllocator
// (SDK 16.0+) to provide zero-copy DMA-backed frame buffers.
class allocator_s : public IDeckLinkVideoBufferAllocator
{
    using transfer_i = gpu::transfer::transfer_i;

    using buffer_ptr_t   = std::unique_ptr<video_buffer_s>;
    using buffer_map_t   = std::map<video_buffer_s*, buffer_ptr_t>;
    using buffer_queue_t = std::deque<buffer_ptr_t>;

    const transfer_i::type_e        transfer_type_;
    const transfer_i::direction_e   direction_;
    std::shared_ptr<gpu::context_s> ctx_;
    uint32_t                        buffer_size_{0};

    buffer_map_t   allocated_buffers_;
    buffer_queue_t free_buffers_;

    std::atomic_ulong ref_count_{1};

    static inline std::atomic_size_t allocations_g{0};

  public:
    allocator_s(std::shared_ptr<gpu::context_s> ctx, gpu::transfer::transfer_i::direction_e dir);
    ~allocator_s();

    // Must be called before the first AllocateVideoBuffer, e.g. from
    // IDeckLinkVideoBufferAllocatorProvider::GetVideoBufferAllocator.
    void set_buffer_size(uint32_t size) { buffer_size_ = size; }

    HRESULT STDMETHODCALLTYPE AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer) override;

    // Called by video_buffer_s::Release() when its ref count reaches zero.
    void return_buffer(video_buffer_s* buffer);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* ppv) override
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // Returns the underlying transfer for a buffer previously obtained from AllocateVideoBuffer.
    static transfer_i* get_transfer(IDeckLinkVideoBuffer* buffer);

    bool register_texture(gpu::texture_s* texture);
    bool unregister_texture(gpu::texture_s* texture);
    bool begin_texture_use(gpu::texture_s* texture);
    bool end_texture_use(gpu::texture_s* texture);
    // The allocator's context must be current and its mutex held by the caller.
    size_t destroy_free_transfers();
};

} // namespace miximus::nodes::decklink::detail
