#pragma once
#include "gpu/types.hpp"
#include "gpu/vertex_attr.hpp"
#include "utils/const_map.hpp"


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
    return utils::const_map_t<std::string_view, vertex_attr>({
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
    });
}

} // namespace miximus::gpu
