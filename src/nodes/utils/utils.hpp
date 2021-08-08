#pragma once
#include "nodes/node.hpp"

#include <memory>

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_framebuffer_node();
std::shared_ptr<node_i> create_framebuffer_to_texture_node();

} // namespace miximus::nodes::utils
