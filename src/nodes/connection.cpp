#include "nodes/connection.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {
std::size_t connection_hash::operator()(const connection_s& c) const
{
    using boost::hash_combine;

    size_t res = 0;

    hash_combine(res, c.from_node);
    hash_combine(res, c.from_interface);
    hash_combine(res, c.to_node);
    hash_combine(res, c.to_interface);

    return res;
}

void to_json(nlohmann::json& j, const connection_s& con)
{
    j = nlohmann::json{
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