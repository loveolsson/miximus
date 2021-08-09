#include "register.hpp"
#include "nodes/composite/composite.hpp"
#include "nodes/debug/debug.hpp"
#include "nodes/decklink/decklink.hpp"
#include "nodes/ffmpeg/ffmpeg.hpp"
#include "nodes/math/math.hpp"
#include "nodes/node.hpp"
#include "nodes/screen/screen.hpp"
#include "nodes/teleprompter/teleprompter.hpp"
#include "nodes/utils/utils.hpp"

namespace miximus::nodes {

void register_nodes(constructor_map_t* map)
{
    // Math nodes
    map->emplace("math_f64", math::create_math_f64_node);
    map->emplace("math_vec2", math::create_math_vec2_node);
    map->emplace("lerp_f64", math::create_lerp_f64_node);
    map->emplace("lerp_vec2", math::create_lerp_vec2_node);
    map->emplace("lerp_rect", math::create_lerp_rect_node);

    // Utility nodes
    map->emplace("vec2", utils::create_vec2_node);
    map->emplace("rect", utils::create_rect_node);
    map->emplace("framebuffer", utils::create_framebuffer_node);
    map->emplace("framebuffer_to_texture", utils::create_framebuffer_to_texture_node);

    // Input nodes
    map->emplace("decklink_input", decklink::create_input_node);
    map->emplace("ffmpeg_player", decklink::create_input_node);

    // Output nodes
    map->emplace("screen_output", screen::create_screen_output_node);
    // map->emplace("decklink_output", decklink::create_output_node);

    // Render nodes
    map->emplace("teleprompter", teleprompter::create_teleprompter_node);

    // Composite nodes
    map->emplace("draw_box", composite::create_draw_box_node);

    // Debug nodes
    map->emplace("sinus_source", debug::create_sinus_source_node);
}

} // namespace miximus::nodes