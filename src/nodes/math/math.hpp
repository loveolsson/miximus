#pragma once
#include "nodes/node.hpp"

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_math_f64_node();
std::shared_ptr<node_i> create_math_vec2_node();
std::shared_ptr<node_i> create_lerp_f64_node();
std::shared_ptr<node_i> create_lerp_vec2_node();
std::shared_ptr<node_i> create_lerp_rect_node();

} // namespace miximus::nodes::math
