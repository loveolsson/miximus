#pragma once
#include "nodes/node.hpp"

#include <memory>

namespace miximus::nodes::ffmpeg {

std::shared_ptr<node_i> create_ffmpeg_player_node();

} // namespace miximus::nodes::ffmpeg
