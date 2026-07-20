#include "register.hpp"

#include <memory>

namespace miximus::nodes::debug {

std::shared_ptr<node_i> create_sinus_source_node();
std::shared_ptr<node_i> create_circle_source_node();

void register_nodes(node_definition_map_t* map)
{
    // Debug nodes
    map->emplace("sinus_source", create_sinus_source_node);
    map->emplace("circle_source", create_circle_source_node);
}

} // namespace miximus::nodes::debug
