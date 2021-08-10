#include "register.hpp"

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_math_f64_node();
std::shared_ptr<node_i> create_math_vec2_node();
std::shared_ptr<node_i> create_lerp_f64_node();
std::shared_ptr<node_i> create_lerp_vec2_node();
std::shared_ptr<node_i> create_lerp_rect_node();

void register_nodes(constructor_map_t* map)
{
    // Math nodes
    map->emplace("math_f64", create_math_f64_node);
    map->emplace("math_vec2", create_math_vec2_node);
    map->emplace("lerp_f64", create_lerp_f64_node);
    map->emplace("lerp_vec2", create_lerp_vec2_node);
    map->emplace("lerp_rect", create_lerp_rect_node);
}

} // namespace miximus::nodes::math
