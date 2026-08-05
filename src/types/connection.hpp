#pragma once
#include "json_contract.hpp"

#include <boost/describe.hpp>

#include <string>

namespace miximus {

struct connection_s
{
    std::string from_node;
    std::string from_interface;
    std::string to_node;
    std::string to_interface;

    auto operator<=>(const connection_s&) const = default;
};

BOOST_DESCRIBE_STRUCT(connection_s, (), (from_node, from_interface, to_node, to_interface))

} // namespace miximus
