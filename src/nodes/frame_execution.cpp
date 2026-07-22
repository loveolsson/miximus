#include "frame_execution.hpp"

#include "core/app_state.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"

namespace miximus::nodes {

std::vector<std::string_view> prepare_all_nodes(core::app_state_s* app, node_map_t& nodes)
{
    std::vector<std::string_view> demanding_nodes;
    demanding_nodes.reserve(nodes.size());
    for (auto& [id, record] : nodes) {
        node_i::prepare_result_s result;
        record.node->prepare(app, record.state, &result);
        if (result.demands_execution) {
            demanding_nodes.emplace_back(id);
        }
    }
    return demanding_nodes;
}

void complete_all_nodes(core::app_state_s* app, node_map_t& nodes)
{
    for (auto& [_, record] : nodes) {
        record.node->complete(app);
    }
}

bool submit_node_once(core::app_state_s*    app,
                      const node_map_t&     nodes,
                      std::string_view      id,
                      submitted_node_set_t& submitted_nodes)
{
    if (!submitted_nodes.emplace(id).second) {
        return false;
    }

    const auto node = nodes.find(id);
    if (node == nodes.end()) {
        submitted_nodes.erase(id);
        return false;
    }

    node->second.node->submit(app, nodes, node->second.state);
    return true;
}

bool execute_node_once(core::app_state_s*   app,
                       const node_map_t&    nodes,
                       std::string_view     id,
                       executed_node_set_t& executed_nodes)
{
    if (!executed_nodes.emplace(id).second) {
        return false;
    }

    const auto node = nodes.find(id);
    if (node == nodes.end()) {
        executed_nodes.erase(id);
        return false;
    }

    node->second.node->execute(app, nodes, node->second.state);
    return true;
}

frame_execution_plan_s::frame_execution_plan_s(node_map_t& nodes, std::span<const std::string_view> demanding_nodes)
    : nodes_(&nodes)
{
    demanding_nodes_.reserve(demanding_nodes.size());
    demanding_nodes_.assign(demanding_nodes.begin(), demanding_nodes.end());
}

void frame_execution_plan_s::submit(core::app_state_s* app) const
{
    for (const auto id : demanding_nodes_) {
        submit_node_once(app, *nodes_, id, app->frame_info.submitted_nodes);
    }
}

void frame_execution_plan_s::execute(core::app_state_s* app) const
{
    for (const auto id : demanding_nodes_) {
        execute_node_once(app, *nodes_, id, app->frame_info.executed_nodes);
    }
}

} // namespace miximus::nodes
