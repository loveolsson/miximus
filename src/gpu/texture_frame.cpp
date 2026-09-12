#include "texture_frame.hpp"

#include "device.hpp"
#include "transfer/detail/transfer_layout.hpp"

namespace miximus::gpu {

texture_frame_s::texture_frame_s(device_s& device, transfer::host_frame_layout_s layout, sampling_e sampling)
    : layout_(layout)
{
    const auto plan    = transfer::detail::make_texture_transfer_plan(layout);
    layout_            = plan.host_layout;
    const auto sharing = device.uses_cuda_transfers() ? resource_sharing_e::cuda : resource_sharing_e::local;
    if (layout_.pixel_format == transfer::host_pixel_format_e::v210) {
        buffer_ = device.create_buffer(plan.packed_buffer_bytes, host_access_e::device_only, 1, sharing);
    } else {
        texture_ =
            texture_s(device, layout_.image_dimensions, format_e::rgba_unorm8, plan.input_order, sampling, sharing);
    }
}
} // namespace miximus::gpu
