#include "register.hpp"

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node();
std::shared_ptr<node_i> create_infinite_multiviewer_node();

void register_nodes(constructor_map_t* map)
{
    // Composite nodes
    map->emplace("draw_box", create_draw_box_node);
    map->emplace("infinite_multiviewer", create_infinite_multiviewer_node);
}

} // namespace miximus::nodes::composite
