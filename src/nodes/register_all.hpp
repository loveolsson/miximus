#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes {

class node_i;

void register_all_nodes(constructor_map_t* map);

} // namespace miximus::nodes
