#include "register.hpp"

#include <memory>

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node();

void register_nodes(node_definition_map_t* map)
{
    // Output nodes
    map->emplace("screen_output", create_screen_output_node);
}

} // namespace miximus::nodes::screen
