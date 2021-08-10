#include "register.hpp"

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_ffmpeg_player_node();

void register_nodes(constructor_map_t* map)
{
    // Input nodes
    map->emplace("ffmpeg_player", create_ffmpeg_player_node);
}

} // namespace miximus::nodes::ffmpeg
