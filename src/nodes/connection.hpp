#pragma once
#include <boost/functional/hash.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <tuple>

namespace miximus::nodes {

struct connection_s
{
    std::string from_node;
    std::string from_interface;
    std::string to_node;
    std::string to_interface;

    auto operator<=>(const connection_s&) const = default;
};

void to_json(nlohmann::json& j, const connection_s& con);
void from_json(const nlohmann::json& j, connection_s& con);

} // namespace miximus::nodes

template <>
struct std::hash<miximus::nodes::connection_s>
{
    std::size_t operator()(const miximus::nodes::connection_s& c) const
    {
        return boost::hash_value(std::tie(c.from_node, c.from_interface, c.to_node, c.to_interface));
    }
};
