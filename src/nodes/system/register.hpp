#pragma once
#include "nodes/node_definition.hpp"

#include <string_view>

namespace miximus::nodes::system {

inline constexpr std::string_view SETTINGS_NODE_ID   = "$app";
inline constexpr std::string_view SETTINGS_NODE_TYPE = "application_settings";

void register_nodes(node_definition_map_t* map);

} // namespace miximus::nodes::system
