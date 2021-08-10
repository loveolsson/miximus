#include "register.hpp"

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_framebuffer_node();
std::shared_ptr<node_i> create_framebuffer_to_texture_node();
std::shared_ptr<node_i> create_rect_node();
std::shared_ptr<node_i> create_vec2_node();

void register_nodes(constructor_map_t* map)
{
    // Utility nodes
    map->emplace("vec2", utils::create_vec2_node);
    map->emplace("rect", utils::create_rect_node);
    map->emplace("framebuffer", utils::create_framebuffer_node);
    map->emplace("framebuffer_to_texture", utils::create_framebuffer_to_texture_node);
}

} // namespace miximus::nodes::utils
