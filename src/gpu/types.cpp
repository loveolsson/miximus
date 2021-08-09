#include "gpu/types.hpp"
#include <nlohmann/json.hpp>

namespace miximus::gpu {

void to_json(nlohmann::json& j, const rect_s& v)
{
    j = {
        {"pos", v.pos},
        {"size", v.size},
    };
}

void from_json(const nlohmann::json& j, rect_s& v)
{
    j.at("pos").get_to(v.pos);
    j.at("size").get_to(v.size);
}

} // namespace miximus::gpu

namespace glm {

void to_json(nlohmann::json& j, const dvec2& v) { j = {v[0], v[1]}; }

void from_json(const nlohmann::json& j, dvec2& v)
{
    j.at(0).get_to(v[0]);
    j.at(1).get_to(v[1]);
}

// void to_json(nlohmann::json& j, const ivec2& v) { j = {v[0], v[1]}; }

// void from_json(const nlohmann::json& j, ivec2& v)
// {
//     j.at(0).get_to(v[0]);
//     j.at(1).get_to(v[1]);
// }

} // namespace glm