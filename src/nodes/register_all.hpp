#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes {

class node_i;

void register_all_nodes(node_definition_map_t* map);

} // namespace miximus::nodes
