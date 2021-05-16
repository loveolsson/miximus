#pragma once
#include <cinttypes>

namespace miximus {
struct vec2;
struct rect;
struct texture;
struct framebuffer;

enum class node_connection_type
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
inline node_connection_type get_connection_type();

template <>
inline node_connection_type get_connection_type<double>()
{
    return node_connection_type::f64;
}

template <>
inline node_connection_type get_connection_type<int64_t>()
{
    return node_connection_type::f64;
}

template <>
inline node_connection_type get_connection_type<vec2>()
{
    return node_connection_type::vec2;
}

template <>
inline node_connection_type get_connection_type<rect>()
{
    return node_connection_type::rect;
}

template <>
inline node_connection_type get_connection_type<texture>()
{
    return node_connection_type::framebuffer;
}

template <typename T>
inline bool accepts_connection_input(node_connection_type o)
{
    return get_connection_type<T>() == o;
}

template <>
inline bool accepts_connection_input<texture>(node_connection_type o)
{
    return o == node_connection_type::texture || o == node_connection_type::framebuffer;
}

} // namespace miximus