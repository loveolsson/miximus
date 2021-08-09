#pragma once
#include "nodes/node.hpp"

#include <memory>

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node();

} // namespace miximus::nodes::composite
