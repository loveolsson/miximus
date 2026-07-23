#pragma once
#include "core/app_state_fwd.hpp"
#include "nodes/frame_execution_fwd.hpp"
#include "nodes/node_map_fwd.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace miximus::nodes {

std::vector<std::string_view> prepare_all_nodes(core::app_state_s* app, node_map_t& nodes);
void                          complete_all_nodes(core::app_state_s* app, node_map_t& nodes);

bool submit_node_once(core::app_state_s* app, const node_map_t& nodes, std::string_view id);

bool execute_node_once(core::app_state_s* app, const node_map_t& nodes, std::string_view id);

void submit_demanding_nodes(core::app_state_s*                app,
                            const node_map_t&                 nodes,
                            std::span<const std::string_view> demanding_nodes);
void execute_demanding_nodes(core::app_state_s*                app,
                             const node_map_t&                 nodes,
                             std::span<const std::string_view> demanding_nodes);

} // namespace miximus::nodes
