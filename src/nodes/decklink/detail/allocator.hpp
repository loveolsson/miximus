#pragma once
#include "gpu/transfer/texture_upload.hpp"
#include "platform_compat.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace miximus::nodes::decklink::detail {

class allocator_s;

// Implements IDeckLinkVideoBuffer with a lease into the asynchronous texture
// upload pool. DeckLink DMA writes directly into backend-owned transfer memory.
class video_buffer_s : public IDeckLinkVideoBuffer
{
    allocator_s*                                         allocator_;
    std::optional<gpu::transfer::texture_upload_lease_s> upload_;
    std::atomic_ulong                                    ref_count_{1};
    uint32_t                                             buffer_size_;
    bool                                                 first_access_{true};

  public:
    video_buffer_s(allocator_s* allocator, uint32_t buffer_size)
        : allocator_(allocator)
        , buffer_size_(buffer_size)
    {
    }

    void         clear_upload() { upload_.reset(); }
    uint64_t     upload_version() const { return upload_ ? upload_->version() : 0; }
    void         submit_upload() { upload_->submit(); }
    void         reset_ref_count() { ref_count_ = 1; }
    allocator_s* allocator() const { return allocator_; }

    HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
    {
        if (buffer == nullptr) {
            return E_POINTER;
        }
        if (!upload_) {
            *buffer = nullptr;
            return E_FAIL;
        }
        *buffer = upload_->bytes().data();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSize(uint64_t* size) override
    {
        if (size == nullptr) {
            return E_POINTER;
        }
        *size = buffer_size_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags flags) override;
    HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags /*flags*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkVideoBuffer>(iid)) {
            *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
            AddRef();
            return S_OK;
        }
        if (decklink_iid_equal(iid, upload_video_buffer_iid())) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() override;
};

class allocator_s : public IDeckLinkVideoBufferAllocator
{
    using buffer_ptr_t   = std::unique_ptr<video_buffer_s>;
    using buffer_map_t   = std::map<video_buffer_s*, buffer_ptr_t>;
    using buffer_queue_t = std::deque<buffer_ptr_t>;

    std::mutex                                              mutex_;
    std::condition_variable                                 idle_condition_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    buffer_map_t                                            allocated_buffers_;
    buffer_queue_t                                          free_buffers_;
    bool                                                    shutting_down_{};
    std::atomic_ulong                                       ref_count_{1};
    uint32_t                                                buffer_size_;

    static inline std::atomic_size_t allocations_g{0};

  public:
    explicit allocator_s(uint32_t buffer_size)
        : buffer_size_(buffer_size)
    {
    }
    ~allocator_s();

    void set_upload_stream(std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream);
    auto acquire_upload(bool first_access) -> std::optional<gpu::transfer::texture_upload_lease_s>;

    HRESULT STDMETHODCALLTYPE AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer) override;
    void                      return_buffer(video_buffer_s* buffer);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkVideoBufferAllocator>(iid)) {
            *ppv = static_cast<IDeckLinkVideoBufferAllocator*>(this);
            AddRef();
            return S_OK;
        }
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

    void shutdown_and_wait();
};

decklink_ptr<video_buffer_s> query_upload_video_buffer(IUnknown* source);

} // namespace miximus::nodes::decklink::detail
