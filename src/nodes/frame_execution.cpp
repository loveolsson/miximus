#include "frame_execution.hpp"

#include "nodes/node.hpp"
#include "nodes/node_map.hpp"

#include <unordered_set>

namespace {
void append_upstream_nodes(miximus::nodes::node_map_t*                           nodes,
                           std::string_view                                      id,
                           std::unordered_set<std::string_view>*                 visited,
                           std::vector<miximus::nodes::frame_execution_entry_s>* active_nodes)
{
    if (!visited->emplace(id).second) {
        return;
    }

    const auto node = nodes->find(id);
    if (node == nodes->end()) {
        return;
    }

    for (const auto& [_, connections] : node->second.state.con_map) {
        for (const auto& connection : connections) {
            if (connection.to_node == id) {
                append_upstream_nodes(nodes, connection.from_node, visited, active_nodes);
            }
        }
    }

    active_nodes->push_back({id, &node->second});
}
} // namespace

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
    active_nodes_.reserve(nodes.size());

    std::unordered_set<std::string_view> visited;
    visited.reserve(nodes.size());

    for (const auto id : demanding_nodes_) {
        append_upstream_nodes(nodes_, id, &visited, &active_nodes_);
    }
}

void frame_execution_plan_s::submit(core::app_state_s* app) const
{
    for (const auto& [_, record] : active_nodes_) {
        record->node->submit(app, *nodes_, record->state);
    }
}

void frame_execution_plan_s::execute(core::app_state_s* app, executed_node_set_t& executed_nodes) const
{
    for (const auto id : demanding_nodes_) {
        execute_node_once(app, *nodes_, id, executed_nodes);
    }
}

} // namespace miximus::nodes
