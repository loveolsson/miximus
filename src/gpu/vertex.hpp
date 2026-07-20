#pragma once
#include "gpu/types.hpp"
#include "gpu/vertex_attr.hpp"

#include <array>
#include <string_view>

namespace miximus::gpu {

struct vertex_uv
{
    glm::vec2 pos;
    glm::vec2 uv;
};

struct vertex_attr_info_s
{
    std::string_view name;
    vertex_attr      attr;
};

template <typename T>
constexpr auto get_vertex_type_info();

template <>
constexpr auto get_vertex_type_info<vertex_uv>()
{
    return std::array{
        vertex_attr_info_s{
                           .name = "pos",
                           .attr =
                vertex_attr{
                    2,
                    GL_FLOAT,
                    GL_FALSE,
                    offsetof(vertex_uv, pos),
                    GL_FLOAT_VEC2,
                }, },
        vertex_attr_info_s{
                           .name = "uv",
                           .attr =
                vertex_attr{
                    2,
                    GL_FLOAT,
                    GL_FALSE,
                    offsetof(vertex_uv, uv),
                    GL_FLOAT_VEC2,
                }, },
    };
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
