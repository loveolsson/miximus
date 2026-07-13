#pragma once
#include "gpu/framebuffer_fwd.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/types.hpp"

#include <string_view>

namespace miximus::nodes {
enum class interface_type_e
{
    f64,
    vec2,
    rect,
    texture,
    framebuffer,
};

template <typename T>
interface_type_e get_interface_type();

template <>
inline interface_type_e get_interface_type<double>()
{
    return interface_type_e::f64;
}

template <>
inline interface_type_e get_interface_type<gpu::vec2_t>()
{
    return interface_type_e::vec2;
}

template <>
inline interface_type_e get_interface_type<gpu::rect_s>()
{
    return interface_type_e::rect;
}

template <>
inline interface_type_e get_interface_type<gpu::texture_s*>()
{
    return interface_type_e::texture;
}

template <>
inline interface_type_e get_interface_type<gpu::framebuffer_s*>()
{
    return interface_type_e::framebuffer;
}

} // namespace miximus::nodes
