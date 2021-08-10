#include "register.hpp"

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node();

void register_nodes(constructor_map_t* map)
{
    // Composite nodes
    map->emplace("draw_box", create_draw_box_node);
}

} // namespace miximus::nodes::composite
