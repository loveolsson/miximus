#include "gpu/types.hpp"
#include <nlohmann/json.hpp>

namespace glm {

void to_json(nlohmann::json& j, const dvec2& v) { nlohmann::json({v[0], v[1]}); }

void from_json(const nlohmann::json& j, dvec2& v)
{
    j[0].get_to(v[0]);
    j[1].get_to(v[1]);
}

} // namespace glm