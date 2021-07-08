#pragma once
#include "gpu/types.hpp"
#include "gpu/vertex_attr.hpp"

#include <frozen/map.h>

#include <array>
#include <string_view>

namespace miximus::gpu {

template <typename T>
constexpr auto get_vertex_type_info();

struct vertex_col_uv
{
    glm::vec2 pos;
    glm::vec2 uv;
};

template <>
inline constexpr auto get_vertex_type_info<vertex_col_uv>()
{
    return frozen::map<std::string_view, vertex_attr, 2>{
        {"pos",
         {
             2,
             GL_FLOAT,
             GL_FALSE,
             offsetof(vertex_col_uv, pos),
             GL_FLOAT_VEC2,
         }},
        {"uv",
         {
             2,
             GL_FLOAT,
             GL_FALSE,
             offsetof(vertex_col_uv, uv),
             GL_FLOAT_VEC2,
         }},
    };
}

} // namespace miximus::gpu
