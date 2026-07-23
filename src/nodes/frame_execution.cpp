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

bool submit_node_once(core::app_state_s* app, const node_map_t& nodes, std::string_view id)
{
    auto& submitted_nodes = app->frame_info.submitted_nodes;
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

bool execute_node_once(core::app_state_s* app, const node_map_t& nodes, std::string_view id)
{
    auto& executed_nodes = app->frame_info.executed_nodes;
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

void submit_demanding_nodes(core::app_state_s*                app,
                            const node_map_t&                 nodes,
                            std::span<const std::string_view> demanding_nodes)
{
    for (const auto id : demanding_nodes) {
        submit_node_once(app, nodes, id);
    }
}

void execute_demanding_nodes(core::app_state_s*                app,
                             const node_map_t&                 nodes,
                             std::span<const std::string_view> demanding_nodes)
{
    for (const auto id : demanding_nodes) {
        execute_node_once(app, nodes, id);
    }
}

} // namespace miximus::nodes
