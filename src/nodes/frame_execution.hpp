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

bool submit_node_once(core::app_state_s*    app,
                      const node_map_t&     nodes,
                      std::string_view      id,
                      submitted_node_set_t& submitted_nodes);

bool execute_node_once(core::app_state_s*   app,
                       const node_map_t&    nodes,
                       std::string_view     id,
                       executed_node_set_t& executed_nodes);

class frame_execution_plan_s
{
    node_map_t*                   nodes_{};
    std::vector<std::string_view> demanding_nodes_;

  public:
    frame_execution_plan_s(node_map_t& nodes, std::span<const std::string_view> demanding_nodes);

    void submit(core::app_state_s* app) const;
    void execute(core::app_state_s* app) const;
};

} // namespace miximus::nodes
