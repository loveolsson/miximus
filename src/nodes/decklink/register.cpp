#include "register.hpp"

namespace miximus::nodes::decklink {

std::shared_ptr<node_i> create_input_node();

void register_nodes(constructor_map_t* map)
{
    // Input nodes
    map->emplace("decklink_input", decklink::create_input_node);
}

} // namespace miximus::nodes::decklink