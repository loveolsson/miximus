#include "register.hpp"

#include <memory>

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_playback_looping_node();

void register_nodes(node_definition_map_t* map)
{
    map->emplace("ffmpeg_playback_looping", create_playback_looping_node);
}

} // namespace miximus::nodes::ffmpeg
