#include "gpu/types.hpp"

#include <string_view>

namespace miximus {
namespace gpu {
class texture_s;
class framebuffer_s;
} // namespace gpu

namespace nodes {
enum class interface_type_e
{
    invalid = -1,
    f64     = 0,
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

} // namespace nodes
} // namespace miximus

inline std::string_view to_string(miximus::nodes::interface_type_e e)
{
    using type_e = miximus::nodes::interface_type_e;

    switch (e) {
        case type_e::invalid:
            return "invalid";
        case type_e::f64:
            return "f64";
        case type_e::vec2:
            return "vec2";
        case type_e::rect:
            return "rect";
        case type_e::texture:
            return "texture";
        case type_e::framebuffer:
            return "framebuffer";
        default:
            return "bad_value";
    }
}