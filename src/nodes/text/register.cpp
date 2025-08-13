#include "register.hpp"

namespace miximus::nodes::text {

std::shared_ptr<node_i> create_text_node();

void register_nodes(constructor_map_t* map)
{
    // Text nodes
    map->emplace("text", text::create_text_node);
}

} // namespace miximus::nodes::text
