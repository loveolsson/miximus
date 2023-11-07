#pragma once
#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <nlohmann/json_fwd.hpp>

namespace miximus::gpu {
using vec2_t  = glm::dvec2;
using vec2i_t = glm::ivec2;
using mat3    = glm::mat3x3;

struct rect_s
{
    vec2_t pos{0, 0};
    vec2_t size{1.0, 1.0};

    constexpr bool operator==(const rect_s& o) const { return pos == o.pos && size == o.size; }
    constexpr bool operator!=(const rect_s& o) const { return !(*this == o); }
};

struct recti_s
{
    vec2i_t pos, size;

    constexpr bool operator==(const recti_s& o) const { return pos == o.pos && size == o.size; }
    constexpr bool operator!=(const recti_s& o) const { return !(*this == o); }
};

void to_json(nlohmann::json& j, const rect_s& v);
void from_json(const nlohmann::json& j, rect_s& v);
} // namespace miximus::gpu

namespace glm {
void to_json(nlohmann::json& j, const dvec2& v);
void from_json(const nlohmann::json& j, dvec2& v);
} // namespace glm