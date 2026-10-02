#pragma once

#include "gpu/texture_fwd.hpp"
#include "nodes/source_name.hpp"
#include "utils/is_finite.hpp"

namespace miximus::nodes {

// Graph values borrow images and may borrow source names for the current frame.
// Producers retain the existing resource/lease lifetime; copies may rename independently.
struct texture_source_info_s
{
    const gpu::texture_s* texture{};
    source_name_s         name;
};

struct framebuffer_source_info_s
{
    gpu::texture_s* texture{};
    source_name_s   name;

    [[nodiscard]] texture_source_info_s as_texture() const { return {.texture = texture, .name = name}; }
};

} // namespace miximus::nodes

namespace miximus::utils {

template <>
inline bool is_finite<nodes::texture_source_info_s>(const nodes::texture_source_info_s& /*value*/) noexcept
{
    return true;
}

template <>
inline bool is_finite<nodes::framebuffer_source_info_s>(const nodes::framebuffer_source_info_s& /*value*/) noexcept
{
    return true;
}

} // namespace miximus::utils
