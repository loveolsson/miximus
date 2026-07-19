#pragma once

#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>

namespace miximus::gpu::transfer {

enum class host_access_e : std::uint8_t
{
    overwrite,
    read_write,
    read_only,
};

struct texture_transfer_requirements_s
{
    vec2i_t             dimensions{};
    texture_s::format_e format{texture_s::format_e::bgra_u8};
    size_t              row_stride{};
    size_t              byte_size{};
    size_t              address_alignment{1};
    host_access_e       host_access{host_access_e::overwrite};
};

} // namespace miximus::gpu::transfer
