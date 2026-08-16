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

struct texture_transfer_layout_s
{
    vec2i_t                   dimensions{};
    texture_s::pixel_format_e pixel_format{texture_s::pixel_format_e::bgra_u8};
    size_t                    host_row_stride_bytes{};
    size_t                    host_buffer_size_bytes{};
    size_t                    host_address_alignment_bytes{1};
    host_memory_access_e      host_memory_access{host_memory_access_e::overwrite};
};

} // namespace miximus::gpu::transfer
