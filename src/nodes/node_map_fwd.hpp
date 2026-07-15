#pragma once
#include "nodes/connection.hpp"
#include "nodes/interface_fwd.hpp"
#include "utils/transparent_string_hash.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace miximus::nodes {

struct node_record_s;
struct node_state_s;

// Set of connections connected to an interface
using con_set_t = std::vector<connection_s>;

// Map of connection sets, keyed by interface name
using con_map_t = std::map<std::string_view, con_set_t>;

// Map of interfaces stored on each node
using interface_map_t = std::map<std::string_view, const interface_i*>;

// Map of node records, keyed by node ID
using node_map_t = std::unordered_map<std::string, node_record_s, utils::transparent_string_hash, std::equal_to<>>;

} // namespace miximus::nodes
