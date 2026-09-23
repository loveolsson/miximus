#include "register.hpp"

#include <memory>

namespace miximus::nodes::cef {
std::shared_ptr<node_i> create_browser_node();
void                    register_nodes(node_definition_map_t* map) { map->emplace("cef_browser", create_browser_node); }
} // namespace miximus::nodes::cef
