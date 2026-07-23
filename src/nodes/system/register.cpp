#include "register.hpp"

#include <memory>

namespace miximus::nodes::system {

std::shared_ptr<node_i> create_settings_node();

void register_nodes(node_definition_map_t* map) { map->emplace(SETTINGS_NODE_TYPE, create_settings_node); }

} // namespace miximus::nodes::system
