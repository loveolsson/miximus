#pragma once
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <tuple>

namespace miximus::nodes {

struct connection
{
    std::string from_node;
    std::string from_interface;
    std::string to_node;
    std::string to_interface;

    auto tie() const { return std::tie(from_node, from_interface, to_node, to_interface); }
    bool operator==(const connection& o) const { return tie() == o.tie(); }

    nlohmann::json serialize();
};

struct connection_hash
{
    std::size_t operator()(const miximus::nodes::connection& c) const;
};

} // namespace miximus::nodes
