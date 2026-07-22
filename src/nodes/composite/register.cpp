#include "register.hpp"

#include <memory>

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node();
std::shared_ptr<node_i> create_infinite_multiviewer_node();
std::shared_ptr<node_i> create_mix_tex_2_node();

void register_nodes(node_definition_map_t* map)
{
    // Composite nodes
    map->emplace("draw_box", create_draw_box_node);
    map->emplace("infinite_multiviewer", create_infinite_multiviewer_node);
    map->emplace("mix_tex_2", create_mix_tex_2_node);
}

} // namespace miximus::nodes::composite
