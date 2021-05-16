#pragma once
#include "utils/const_map.hpp"

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace miximus::gpu {

struct vertex_attr
{
    GLint     size;
    GLenum    type;
    GLboolean norm;
    GLsizei   offset;
};

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
         }},
        {"uv",
         {
             2,
             GL_FLOAT,
             GL_FALSE,
             offsetof(vertex_col_uv, uv),
         }},
    });
}

} // namespace miximus::gpu
