#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes::dummy {

std::shared_ptr<node> create_node(const std::string& id);

} // namespace miximus::nodes::dummy