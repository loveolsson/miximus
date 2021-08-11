#include "register.hpp"

namespace miximus::nodes::decklink {

std::shared_ptr<node_i> create_input_node();
std::shared_ptr<node_i> create_output_node();

void register_nodes(constructor_map_t* map)
{
    // Input nodes
    map->emplace("decklink_input", decklink::create_input_node);

    // Output nodes
    map->emplace("decklink_output", decklink::create_output_node);
}

} // namespace miximus::nodes::decklink