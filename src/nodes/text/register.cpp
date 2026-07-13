#include "register.hpp"

#include <memory>

namespace miximus::nodes::text {

std::shared_ptr<node_i> create_text_node();

void register_nodes(node_definition_map_t* map)
{
    // Text nodes
    map->emplace("text", text::create_text_node);
}

} // namespace miximus::nodes::text
