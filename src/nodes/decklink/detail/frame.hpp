#pragma once
#include "wrapper/decklink-sdk/decklink_inc.hpp"

namespace miximus::nodes::decklink::detail {

class decklink_frame_s : public IDeckLinkVideoFrame
{
    std::atomic_ulong    ref_count_{1};
    void* const          data_;
    const long           width_;
    const long           height_;
    const long           row_bytes_;
    const BMDPixelFormat pixel_format_;
    const BMDFrameFlags  flags_;

  public:
    decklink_frame_s(void* const    data,
                     long           width,
                     long           height,
                     long           row_bytes,
                     BMDPixelFormat pixel_format = bmdFormat8BitARGB,
                     BMDFrameFlags  flags        = 0)
        : data_(data)
        , width_(width)
        , height_(height)
        , row_bytes_(row_bytes)
        , pixel_format_(pixel_format)
        , flags_(flags)
    {
    }

    /**
     * IUnknown
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/) final { return E_NOTIMPL; }

    ULONG STDMETHODCALLTYPE AddRef() final { return ++ref_count_; }

    ULONG STDMETHODCALLTYPE Release() final
    {
        ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    /**
     * IDeckLinkVideoFrame
     */
    long           GetWidth() final { return width_; }
    long           GetHeight() final { return height_; }
    long           GetRowBytes() final { return row_bytes_; }
    BMDPixelFormat GetPixelFormat() final { return pixel_format_; }
    BMDFrameFlags  GetFlags() final { return flags_; }

    HRESULT GetBytes(void** buffer) final
    {
        *buffer = data_;
        return S_OK;
    }

    HRESULT GetTimecode(BMDTimecodeFormat /*format*/, IDeckLinkTimecode** /*timecode*/) final
    {
        return E_NOTIMPL; //
    }

    HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary** ancillary) final
    {
        return E_NOTIMPL; //
    }
};

} // namespace miximus::nodes::decklink::detail