#include "register.hpp"

#include <memory>

namespace miximus::nodes::switch_nodes {

std::shared_ptr<node_i> create_switch_f64_4_node();
std::shared_ptr<node_i> create_switch_f64_8_node();
std::shared_ptr<node_i> create_switch_vec2_4_node();
std::shared_ptr<node_i> create_switch_vec2_8_node();
std::shared_ptr<node_i> create_switch_rect_4_node();
std::shared_ptr<node_i> create_switch_rect_8_node();
std::shared_ptr<node_i> create_switch_tex_4_node();
std::shared_ptr<node_i> create_switch_tex_8_node();

void register_nodes(node_definition_map_t* map)
{
    map->emplace("switch_f64_4", create_switch_f64_4_node);
    map->emplace("switch_f64_8", create_switch_f64_8_node);
    map->emplace("switch_vec2_4", create_switch_vec2_4_node);
    map->emplace("switch_vec2_8", create_switch_vec2_8_node);
    map->emplace("switch_rect_4", create_switch_rect_4_node);
    map->emplace("switch_rect_8", create_switch_rect_8_node);
    map->emplace("switch_tex_4", create_switch_tex_4_node);
    map->emplace("switch_tex_8", create_switch_tex_8_node);
}

} // namespace miximus::nodes::switch_nodes
