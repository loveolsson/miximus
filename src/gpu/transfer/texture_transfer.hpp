#pragma once

#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>

namespace miximus::gpu::transfer {

enum class host_memory_access_e : std::uint8_t
{
    overwrite,
    read_write,
    read_only,
};

enum class host_pixel_format_e : std::uint8_t
{
    rgba_u8,
    bgra_u8,
    bgrx_u8,
    argb_u8,
    v210,
};

struct host_frame_layout_s
{
    vec2i_t              image_dimensions{};
    host_pixel_format_e  pixel_format{host_pixel_format_e::rgba_u8};
    size_t               row_stride_bytes{};
    size_t               buffer_size_bytes{};
    size_t               address_alignment_bytes{1};
    host_memory_access_e memory_access{host_memory_access_e::overwrite};
};

} // namespace miximus::gpu::transfer
