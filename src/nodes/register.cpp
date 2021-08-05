#include "register.hpp"
#include "nodes/debug/debug.hpp"
#include "nodes/decklink/decklink.hpp"
#include "nodes/math/math.hpp"
#include "nodes/node.hpp"
#include "nodes/screen/screen.hpp"
#include "nodes/teleprompter/teleprompter.hpp"
#include "nodes/utils/utils.hpp"

namespace miximus::nodes {

void register_nodes(constructor_map_t* map)
{
    // Math nodes
    map->emplace("math_i64", math::create_i64_node);
    map->emplace("math_f64", math::create_f64_node);
    map->emplace("math_vec2", math::create_vec2_node);
    map->emplace("math_vec2i", math::create_vec2i_node);

    // Utility nodes
    map->emplace("framebuffer", utils::create_framebuffer_node);

    // Input nodes
    map->emplace("decklink_input", decklink::create_input_node);

    // Output nodes
    map->emplace("screen_output", screen::create_screen_output_node);
    // map->emplace("decklink_output", decklink::create_output_node);

    // Render nodes
    map->emplace("teleprompter", teleprompter::create_teleprompter_node);

    // Debug nodes
    map->emplace("sinus_source", debug::create_sinus_source_node);
}

} // namespace miximus::nodes