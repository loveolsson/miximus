#pragma once

#include "external_image_win.hpp"
#include "gpu/video_frame.hpp"

namespace miximus::gpu::detail {

// Worker-owned, reusable raw video ingress. The producer must finish writing
// before convert(), and may reuse its image only after the returned completion.
// No pixel storage is host visible; only the small component descriptor is sent
// with the commands. Color conversion uses the ordinary video component shader.
class external_video_copy_s
{
    std::shared_ptr<texture_state_s> image_;
    buffer_s                         components_;

  public:
    // Independent decoder context on the compositor adapter, sharing CEF's LUID selection logic.
    static Microsoft::WRL::ComPtr<ID3D11Device> create_producer_device(device_s& device);
    external_video_copy_s(device_s& device, const d3d11_image_description_s& image, size_t bytes);
    completion_s convert(recording_s&                 record,
                         const texture_s&             target,
                         const video_buffer_header_s& header,
                         const color_transform_s&     color);
};

} // namespace miximus::gpu::detail
