#pragma once

#include "external_image_win.hpp"
#include "gpu/video_frame.hpp"

#include <d3d11.h>

namespace miximus::gpu::detail {

// Preserve raw NV12/P010/P016 components in a single-plane shared RGBA8 carrier.
// This avoids relying on cross-API agreement about opaque multi-plane tiling.
// No YUV/RGB conversion occurs here. The consumer still uses unpack_video.
class d3d11_video_pack_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    d3d11_video_pack_s(ID3D11Device*                device,
                       const D3D11_TEXTURE2D_DESC&  source,
                       const video_buffer_header_s& header,
                       size_t                       bytes,
                       extent_s                     visible);
    ~d3d11_video_pack_s();
    d3d11_video_pack_s(const d3d11_video_pack_s&)            = delete;
    d3d11_video_pack_s& operator=(const d3d11_video_pack_s&) = delete;
    d3d11_video_pack_s(d3d11_video_pack_s&&)                 = delete;
    d3d11_video_pack_s& operator=(d3d11_video_pack_s&&)      = delete;

    // Caller serializes immediate-context access with the producer's lock.
    // Caller must establish GPU completion before the consumer reads the handle.
    void                      copy(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT slice);
    d3d11_image_description_s image() const;
    size_t                    buffer_bytes() const;
};

} // namespace miximus::gpu::detail
