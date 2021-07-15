#pragma once
#include "gpu/types.hpp"
#include <cinttypes>

namespace miximus {
namespace gpu {
struct texture;
struct framebuffer;
} // namespace gpu

namespace nodes {
enum class interface_type_e
{
    invalid = -1,
    f64     = 0,
    i64,
    vec2,
    rect,
    texture,
    framebuffer,
};

template <typename T>
constexpr interface_type_e get_interface_type();

template <>
constexpr interface_type_e get_interface_type<double>()
{
    return interface_type_e::f64;
}

template <>
constexpr interface_type_e get_interface_type<int64_t>()
{
    return interface_type_e::i64;
}

template <>
constexpr interface_type_e get_interface_type<gpu::vec2>()
{
    return interface_type_e::vec2;
}

template <>
constexpr interface_type_e get_interface_type<gpu::rect>()
{
    return interface_type_e::rect;
}

template <>
constexpr interface_type_e get_interface_type<gpu::texture>()
{
    return interface_type_e::framebuffer;
}

} // namespace nodes
} // namespace miximus