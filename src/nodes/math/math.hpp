#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_i64_node();
std::shared_ptr<node_i> create_f64_node();
std::shared_ptr<node_i> create_vec2_node();

} // namespace miximus::nodes::math
