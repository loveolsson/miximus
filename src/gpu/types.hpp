#pragma once
#include <glm/vec2.hpp>

namespace miximus::gpu {
using vec2 = glm::dvec2;

struct rect
{
    vec2 pos, size;
};

} // namespace miximus::gpu