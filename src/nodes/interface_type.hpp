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
[[nodiscard]] constexpr interface_type_e get_interface_type() noexcept;

template <>
[[nodiscard]] constexpr interface_type_e get_interface_type<double>() noexcept
{
    return interface_type_e::f64;
}

template <>
[[nodiscard]] constexpr interface_type_e get_interface_type<gpu::vec2_t>() noexcept
{
    return interface_type_e::vec2;
}

template <>
[[nodiscard]] constexpr interface_type_e get_interface_type<gpu::rect_s>() noexcept
{
    return interface_type_e::rect;
}

template <>
[[nodiscard]] constexpr interface_type_e get_interface_type<gpu::texture_s*>() noexcept
{
    return interface_type_e::texture;
}

template <>
[[nodiscard]] constexpr interface_type_e get_interface_type<gpu::framebuffer_s*>() noexcept
{
    return interface_type_e::framebuffer;
}

} // namespace miximus::nodes
