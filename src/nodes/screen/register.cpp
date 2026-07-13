#include "register.hpp"

#include "screen_output_migrations.hpp"

#include <memory>

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node();

void register_nodes(node_definition_map_t* map)
{
    // Output nodes
    map->emplace("screen_output", node_definition_s{create_screen_output_node, screen_output_migrations()});
}

} // namespace miximus::nodes::screen
