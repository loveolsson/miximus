#pragma once
#include "gpu/transfer/texture_upload.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace miximus::nodes::decklink::detail {

class input_video_buffer_allocator_s;

// Implements IDeckLinkVideoBuffer with a lease into the asynchronous texture
// upload pool. DeckLink DMA writes directly into backend-owned transfer memory.
class input_video_buffer_s final : public IDeckLinkVideoBuffer
{
    input_video_buffer_allocator_s*                      allocator_;
    std::optional<gpu::transfer::texture_upload_lease_s> upload_;
    std::atomic_ulong                                    ref_count_{1};
    uint32_t                                             buffer_size_;
    bool                                                 first_access_{true};

    friend class input_video_buffer_allocator_s;

    void activate() { ref_count_ = 1; }
    void clear_upload() { upload_.reset(); }

  public:
    input_video_buffer_s(input_video_buffer_allocator_s* allocator, uint32_t buffer_size)
        : allocator_(allocator)
        , buffer_size_(buffer_size)
    {
    }

    auto take_upload() -> std::optional<gpu::transfer::texture_upload_lease_s>
    {
        return std::exchange(upload_, std::nullopt);
    }
    input_video_buffer_allocator_s* allocator() const noexcept { return allocator_; }

    HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) noexcept override
    {
        if (buffer == nullptr) {
            return E_POINTER;
        }
        if (!upload_) {
            *buffer = nullptr;
            return E_FAIL;
        }
        *buffer = upload_->writable_host_bytes().data();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSize(uint64_t* size) noexcept override
    {
        if (size == nullptr) {
            return E_POINTER;
        }
        *size = buffer_size_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags flags) noexcept override;
    HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags /*flags*/) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) noexcept override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_sdk::decklink_iid_matches<IUnknown>(iid) ||
            decklink_sdk::decklink_iid_matches<IDeckLinkVideoBuffer>(iid)) {
            *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
            AddRef();
            return S_OK;
        }
        if (decklink_sdk::decklink_iid_equal(iid, decklink_sdk::input_video_buffer_iid())) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override;
};

class input_video_buffer_allocator_s final : public IDeckLinkVideoBufferAllocator
{
  public:
    // DeckLink treats E_OUTOFMEMORY as the end of the capture-buffer pool.
    // Keep SDK-owned buffers bounded independently of transfer-slot lifetime.
    static constexpr size_t BUFFER_COUNT              = 8;
    static constexpr size_t INITIAL_UPLOAD_SLOT_COUNT = BUFFER_COUNT;
    static constexpr size_t UPLOAD_SLOT_COUNT         = 16;

  private:
    struct buffer_slot_s
    {
        std::unique_ptr<input_video_buffer_s> buffer;
        bool                                  active{};
    };

    std::mutex                                              mutex_;
    std::condition_variable                                 idle_condition_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    std::array<buffer_slot_s, BUFFER_COUNT>                 buffers_;
    size_t                                                  active_buffers_{};
    bool                                                    shutting_down_{};
    std::atomic_uint64_t                                    upload_acquire_slow_count_{};
    std::atomic_uint64_t                                    upload_acquire_failures_{};
    std::atomic_uint64_t                                    upload_acquire_wait_max_us_{};
    std::atomic_ulong                                       ref_count_{1};
    uint32_t                                                buffer_size_;

  public:
    input_video_buffer_allocator_s(uint32_t                                                buffer_size,
                                   std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream)
        : upload_stream_(std::move(upload_stream))
        , buffer_size_(buffer_size)
    {
    }
    ~input_video_buffer_allocator_s();

    auto     acquire_upload(bool first_access) -> std::optional<gpu::transfer::texture_upload_lease_s>;
    auto     buffer_size() const noexcept { return buffer_size_; }
    uint64_t upload_acquire_slow_count() const noexcept { return upload_acquire_slow_count_.load(); }
    uint64_t upload_acquire_failures() const noexcept { return upload_acquire_failures_.load(); }
    uint64_t upload_acquire_wait_max_us() const noexcept { return upload_acquire_wait_max_us_.load(); }

    HRESULT STDMETHODCALLTYPE AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer) noexcept override;
    void                      return_buffer(input_video_buffer_s* buffer);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) noexcept override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_sdk::decklink_iid_matches<IUnknown>(iid) ||
            decklink_sdk::decklink_iid_matches<IDeckLinkVideoBufferAllocator>(iid)) {
            *ppv = static_cast<IDeckLinkVideoBufferAllocator*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void shutdown_and_wait();
};

decklink_sdk::decklink_ptr<input_video_buffer_s> query_input_video_buffer(IUnknown* source);

} // namespace miximus::nodes::decklink::detail
