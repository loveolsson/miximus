#pragma once
#include <glm/vec2.hpp>
#include <nlohmann/json_fwd.hpp>

#include <tuple>

namespace miximus::gpu {
using vec2_t  = glm::dvec2;
using vec2i_t = glm::ivec2;

struct rect_s
{
    vec2_t pos, size;

    auto tie() const { return std::tie(pos, size); }
    bool operator==(const rect_s& o) const { return tie() == o.tie(); }
    bool operator!=(const rect_s& o) const { return tie() != o.tie(); }
};

struct recti_s
{
    vec2i_t pos, size;

    auto tie() const { return std::tie(pos, size); }
    bool operator==(const recti_s& o) const { return tie() == o.tie(); }
    bool operator!=(const recti_s& o) const { return tie() != o.tie(); }
};

} // namespace miximus::gpu

namespace glm {
void to_json(nlohmann::json& j, const dvec2& v);
void from_json(const nlohmann::json& j, dvec2& v);
void to_json(nlohmann::json& j, const ivec2& v);
void from_json(const nlohmann::json& j, ivec2& v);
} // namespace glm