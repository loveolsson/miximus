#include "register_all.hpp"
#include "nodes/composite/register.hpp"
#include "nodes/debug/register.hpp"
#include "nodes/decklink/register.hpp"
#include "nodes/ffmpeg/register.hpp"
#include "nodes/math/register.hpp"
#include "nodes/screen/register.hpp"
#include "nodes/teleprompter/register.hpp"
#include "nodes/text/register.hpp"
#include "nodes/utils/register.hpp"

namespace miximus::nodes {

void register_all_nodes(constructor_map_t* map)
{
    math::register_nodes(map);
    utils::register_nodes(map);
    decklink::register_nodes(map);
    ffmpeg::register_nodes(map);
    screen::register_nodes(map);
    teleprompter::register_nodes(map);
    text::register_nodes(map);
    composite::register_nodes(map);
    debug::register_nodes(map);
}

} // namespace miximus::nodes