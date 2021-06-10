#pragma once
#include <cinttypes>
#include <glm/fwd.hpp>

namespace miximus {
namespace gpu {
using vec2 = glm::dvec2;
struct rect;
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
inline interface_type_e get_interface_type();

template <>
inline interface_type_e get_interface_type<double>()
{
    return interface_type_e::f64;
}

template <>
inline interface_type_e get_interface_type<int64_t>()
{
    return interface_type_e::f64;
}

template <>
inline interface_type_e get_interface_type<gpu::vec2>()
{
    return interface_type_e::vec2;
}

template <>
inline interface_type_e get_interface_type<gpu::rect>()
{
    return interface_type_e::rect;
}

template <>
inline interface_type_e get_interface_type<gpu::texture>()
{
    return interface_type_e::framebuffer;
}

bool test_interface_pair(interface_type_e from, interface_type_e to);

} // namespace nodes
} // namespace miximus