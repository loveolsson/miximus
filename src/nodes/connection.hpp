#pragma once
#include "nodes/connection_type.hpp"

#include <string>

namespace miximus::nodes {

struct connection
{
    std::string from_node;
    std::string from_interface;
    std::string to_node;
    std::string to_interface;
};

} // namespace miximus::nodes

namespace std {
template <>
struct hash<miximus::nodes::connection>
{
    std::size_t operator()(const miximus::nodes::connection& c) const;
};
} // namespace std