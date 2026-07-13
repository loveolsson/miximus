#include "register.hpp"

#include <memory>

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_ffmpeg_player_node();

void register_nodes(node_definition_map_t* map)
{
    // Input nodes
    map->emplace("ffmpeg_player", create_ffmpeg_player_node);
}

} // namespace miximus::nodes::ffmpeg
