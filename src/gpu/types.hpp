#pragma once
#include <glm/vec2.hpp>
#include <nlohmann/json_fwd.hpp>

namespace miximus::gpu {
using vec2 = glm::dvec2;

struct rect
{
    vec2 pos, size;
};

} // namespace miximus::gpu

namespace glm {
void to_json(nlohmann::json& j, const dvec2& con);
void from_json(const nlohmann::json& j, dvec2& con);
} // namespace glm