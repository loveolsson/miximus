#pragma once
#include "gpu/types.hpp"
#include "gpu/vertex_attr.hpp"

#include <frozen/map.h>

#include <array>
#include <string_view>

namespace miximus::gpu {

struct vertex_uv
{
    glm::vec2 pos;
    glm::vec2 uv;
};

template <typename T>
constexpr auto get_vertex_type_info();

template <>
constexpr auto get_vertex_type_info<vertex_uv>()
{
    return frozen::make_map<std::string_view, vertex_attr>({
        {"pos",
         {
             2,
             GL_FLOAT,
             GL_FALSE,
             offsetof(vertex_uv, pos),
             GL_FLOAT_VEC2,
         }},
        {"uv",
         {
             2,
             GL_FLOAT,
             GL_FALSE,
             offsetof(vertex_uv, uv),
             GL_FLOAT_VEC2,
         }},
    });
}

constexpr auto full_screen_quad_verts = std::array{
    vertex_uv{{0, 1.f},   {0, 0}    },
    vertex_uv{{1.f, 1.f}, {1.f, 0}  },
    vertex_uv{{0, 0},     {0, 1.f}  },
    vertex_uv{{0, 0},     {0, 1.f}  },
    vertex_uv{{1.f, 1.f}, {1.f, 0}  },
    vertex_uv{{1.f, 0},   {1.f, 1.f}},
};

constexpr auto full_screen_quad_verts_flip_uv = std::array{
    vertex_uv{{0, 1.f},   {0, 1.f}  },
    vertex_uv{{1.f, 1.f}, {1.f, 1.f}},
    vertex_uv{{0, 0},     {0, 0}    },
    vertex_uv{{0, 0},     {0, 0}    },
    vertex_uv{{1.f, 1.f}, {1.f, 1.f}},
    vertex_uv{{1.f, 0},   {1.f, 0}  },
};

} // namespace miximus::gpu
