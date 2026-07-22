#include "register.hpp"

#include <memory>

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_math_f64_node();
std::shared_ptr<node_i> create_math_vec2_node();
std::shared_ptr<node_i> create_lerp_f64_node();
std::shared_ptr<node_i> create_lerp_vec2_node();
std::shared_ptr<node_i> create_lerp_rect_node();
std::shared_ptr<node_i> create_easing_f64_node();

void register_nodes(node_definition_map_t* map)
{
    // Math nodes
    map->emplace("math_f64", create_math_f64_node);
    map->emplace("math_vec2", create_math_vec2_node);
    map->emplace("lerp_f64", create_lerp_f64_node);
    map->emplace("lerp_vec2", create_lerp_vec2_node);
    map->emplace("lerp_rect", create_lerp_rect_node);
    map->emplace("easing_f64", create_easing_f64_node);
}

} // namespace miximus::nodes::math
