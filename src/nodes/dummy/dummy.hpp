#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes::dummy {

std::shared_ptr<node_i> create_node(node_i::type_e type);

} // namespace miximus::nodes::dummy
