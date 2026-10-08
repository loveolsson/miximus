#include "register.hpp"

#include <memory>

namespace miximus::nodes::generators {

std::shared_ptr<node_i> create_test_pattern_node();
std::shared_ptr<node_i> create_image_node();

void register_nodes(node_definition_map_t* map)
{
    map->emplace("test_pattern", create_test_pattern_node);
    map->emplace("image", create_image_node);
}

} // namespace miximus::nodes::generators
