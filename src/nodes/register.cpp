#include "register.hpp"
#include "nodes/decklink/decklink.hpp"
#include "nodes/math/math.hpp"
#include "nodes/node.hpp"
#include "nodes/screen/screen.hpp"
#include "nodes/utils/utils.hpp"

namespace miximus::nodes {

void register_nodes(constructor_map_t* map)
{
    using namespace nodes;

    auto reg = [map](std::string_view name, constructor_t&& c) { map->emplace(name, std::move(c)); };

    reg("math_i64", math::create_i64_node);
    reg("math_f64", math::create_f64_node);
    reg("math_vec2", math::create_vec2_node);
    reg("math_vec2i", math::create_vec2i_node);

    reg("framebuffer", utils::create_framebuffer_node);

    reg("screen_output", screen::create_node);

    reg("decklink_input", decklink::create_input_node);
    // register_node_type("decklink_output", decklink::create_output_node);
}

} // namespace miximus::nodes