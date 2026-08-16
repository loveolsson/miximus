#pragma once
#include "gpu/types.hpp"

#include <array>

namespace miximus::gpu {

struct vertex_uv
{
    glm::vec2 pos;
    glm::vec2 uv;
};

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
