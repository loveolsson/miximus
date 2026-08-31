#pragma once
#include "nodes/interface_fwd.hpp"
#include "types/connection.hpp"
#include "utils/string_map.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::nodes {

struct node_record_s;
struct node_state_s;

// Set of connections connected to an interface
using con_set_t = std::vector<connection_s>;

// Map of connection sets, keyed by interface name
using con_map_t = utils::string_view_map_t<con_set_t>;

// Map of interfaces stored on each node
using interface_map_t = utils::string_view_map_t<const interface_i*>;

// Map of node records, keyed by node ID
using node_map_t = utils::unordered_string_map_t<node_record_s>;

} // namespace miximus::nodes
