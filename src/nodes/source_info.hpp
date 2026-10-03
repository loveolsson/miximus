#pragma once

#include "gpu/texture_fwd.hpp"
#include "utils/is_finite.hpp"

#include <string_view>

namespace miximus::nodes {

// Graph values borrow images and names through the current frame's completion.
// Names reference the active settings or producer-owned storage with that lifetime.
// Consumers retaining a name beyond the frame must copy it into a std::string.
struct texture_source_info_s
{
    const gpu::texture_s* texture{};
    std::string_view      name;
};

struct framebuffer_source_info_s
{
    gpu::texture_s*  texture{};
    std::string_view name;

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
