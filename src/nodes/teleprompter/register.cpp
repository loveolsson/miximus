#include "register.hpp"

namespace miximus::nodes::teleprompter {

std::shared_ptr<node_i> create_teleprompter_node();

void register_nodes(constructor_map_t* map)
{
    // Render nodes
    map->emplace("teleprompter", teleprompter::create_teleprompter_node);
}

} // namespace miximus::nodes::teleprompter
