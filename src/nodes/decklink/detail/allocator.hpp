#pragma once
#include "gpu/transfer/texture_upload.hpp"
#include "platform_compat.hpp"

#include <atomic>
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

  public:
    video_buffer_s(allocator_s* allocator, gpu::transfer::texture_upload_lease_s upload)
        : allocator_(allocator)
        , upload_(std::move(upload))
    {
    }

    void     set_upload(gpu::transfer::texture_upload_lease_s upload) { upload_.emplace(std::move(upload)); }
    void     clear_upload() { upload_.reset(); }
    uint64_t upload_version() const { return upload_ ? upload_->version() : 0; }
    void     submit_upload() { upload_->submit(); }
    uint32_t buffer_size() const { return upload_ ? static_cast<uint32_t>(upload_->bytes().size()) : 0; }
    void     reset_ref_count() { ref_count_ = 1; }

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
        *size = buffer_size();
        return upload_ ? S_OK : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags /*flags*/) override { return S_OK; }
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
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    buffer_map_t                                            allocated_buffers_;
    buffer_queue_t                                          free_buffers_;
    bool                                                    shutting_down_{};
    std::atomic_ulong                                       ref_count_{1};

    static inline std::atomic_size_t allocations_g{0};

  public:
    allocator_s() = default;
    ~allocator_s();

    void set_upload_stream(std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream);

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

    uint64_t upload_version(IDeckLinkVideoBuffer* buffer);
    bool     submit_upload(IDeckLinkVideoBuffer* buffer);

    size_t destroy_free_buffers();
};

} // namespace miximus::nodes::decklink::detail
