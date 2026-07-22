#pragma once
#include "gpu/transfer/texture_download.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace miximus::nodes::decklink::detail {

// Exposes one completed GPU download lease directly to a DeckLink output
// frame. The lease returns to its stream only after DeckLink releases every
// frame reference using this buffer.
class output_video_buffer_s final : public IDeckLinkVideoBuffer
{
    gpu::transfer::texture_download_frame_s frame_;
    std::atomic_ulong                       ref_count_{1};

  public:
    explicit output_video_buffer_s(gpu::transfer::texture_download_frame_s frame)
        : frame_(std::move(frame))
    {
    }

    HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) final
    {
        if (buffer == nullptr) {
            return E_POINTER;
        }
        const auto bytes = frame_.bytes();
        *buffer          = const_cast<std::byte*>(bytes.data());
        return bytes.empty() ? E_FAIL : S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSize(uint64_t* size) final
    {
        if (size == nullptr) {
            return E_POINTER;
        }
        *size = static_cast<uint64_t>(frame_.bytes().size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags /*flags*/) final { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags /*flags*/) final { return S_OK; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
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
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() final { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() final
    {
        const ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }
};

} // namespace miximus::nodes::decklink::detail
