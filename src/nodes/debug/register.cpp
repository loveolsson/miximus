#include "register.hpp"

namespace miximus::nodes::debug {

std::shared_ptr<node_i> create_sinus_source_node();

void register_nodes(constructor_map_t* map)
{
    // Debug nodes
    map->emplace("sinus_source", create_sinus_source_node);
}

} // namespace miximus::nodes::debug
