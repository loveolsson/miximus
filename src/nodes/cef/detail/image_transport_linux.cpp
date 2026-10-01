#include "image_transport.hpp"

#include <stdexcept>

namespace miximus::nodes::cef::detail {

gpu::detail::external_image_s capture_image(const CefAcceleratedPaintInfo& info)
{
    if (info.plane_count != 1 || (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) {
        throw std::runtime_error("Unsupported CEF accelerated image format");
    }
    return {
        .handle   = info.planes[0].fd,
        .extent   = {.width  = static_cast<uint32_t>(info.extra.coded_size.width),
                     .height = static_cast<uint32_t>(info.extra.coded_size.height)},
        .order    = info.format == CEF_COLOR_TYPE_BGRA_8888 ? gpu::channel_order_e::bgra : gpu::channel_order_e::rgba,
        .modifier = info.modifier,
        .offset   = info.planes[0].offset,
        .stride   = info.planes[0].stride
    };
}

void set_media_frame_image(cef_wrapper::media_frame_s& packet, const gpu::detail::external_image_s& image, size_t bytes)
{
    packet.fd               = image.handle;
    packet.stride           = static_cast<uint32_t>(image.stride);
    packet.offset           = image.offset;
    packet.modifier         = image.modifier;
    packet.width            = image.extent.width;
    packet.height           = image.extent.height;
    packet.allocation_bytes = bytes;
}

} // namespace miximus::nodes::cef::detail
