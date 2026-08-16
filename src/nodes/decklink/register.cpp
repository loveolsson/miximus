#include "register.hpp"

#include "output_migrations.hpp"

#include <memory>

namespace miximus::nodes::decklink {

std::shared_ptr<node_i> create_input_node();
std::shared_ptr<node_i> create_output_node();

void register_nodes(node_definition_map_t* map)
{
    // Input nodes
    map->emplace("decklink_input", decklink::create_input_node);

    // Output nodes
    map->emplace("decklink_output", node_definition_s{decklink::create_output_node, output_migrations()});
}

} // namespace miximus::nodes::decklink
