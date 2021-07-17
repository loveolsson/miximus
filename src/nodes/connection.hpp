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

    auto tie() const { return std::tie(from_node, from_interface, to_node, to_interface); }
    bool operator==(const connection_s& o) const { return tie() == o.tie(); }
};

struct connection_hash
{
    std::size_t operator()(const connection_s& c) const;
};

void to_json(nlohmann::json& j, const connection_s& con);
void from_json(const nlohmann::json& j, connection_s& con);

} // namespace miximus::nodes
