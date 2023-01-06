#include "nodes/connection.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

void to_json(nlohmann::json& j, const connection_s& con)
{
    j = {
        {"from_node", con.from_node},
        {"from_interface", con.from_interface},
        {"to_node", con.to_node},
        {"to_interface", con.to_interface},
    };
}

void from_json(const nlohmann::json& j, connection_s& con)
{
    j.at("from_node").get_to(con.from_node);
    j.at("from_interface").get_to(con.from_interface);
    j.at("to_node").get_to(con.to_node);
    j.at("to_interface").get_to(con.to_interface);
}
} // namespace miximus::nodes