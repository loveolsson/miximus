#include "frame_execution.hpp"

#include "nodes/execution_context.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"

namespace miximus::nodes {

std::vector<std::string_view> prepare_all_nodes(execution_context_s* context, node_map_t& nodes)
{
    std::vector<std::string_view> demanding_nodes;
    demanding_nodes.reserve(nodes.size());
    for (auto& [id, record] : nodes) {
        node_i::prepare_result_s result;
        record.node->prepare(context, record.state, &result);
        if (result.demands_execution) {
            demanding_nodes.emplace_back(id);
        }
    }
    return demanding_nodes;
}

void complete_all_nodes(execution_context_s* context, node_map_t& nodes)
{
    for (auto& [_, record] : nodes) {
        record.node->complete(context);
    }
    // Discard all frame-local image pointers and name borrows after completion.
    // Clearing does not dereference a borrowed name, so node order is irrelevant.
    for (const auto& [_, record] : nodes) {
        for (const auto& [name, iface] : record.node->get_interfaces()) {
            iface->clear_frame_value();
        }
    }
}

bool submit_node_once(execution_context_s* context, const node_map_t& nodes, std::string_view id)
{
    auto& submitted_nodes = context->frame_info.submitted_nodes;
    if (!submitted_nodes.emplace(id).second) {
        return false;
    }

    const auto node = nodes.find(id);
    if (node == nodes.end()) {
        submitted_nodes.erase(id);
        return false;
    }

    node->second.node->submit(context, nodes, node->second.state);
    return true;
}

bool execute_node_once(execution_context_s* context, const node_map_t& nodes, std::string_view id)
{
    auto& executed_nodes = context->frame_info.executed_nodes;
    if (!executed_nodes.emplace(id).second) {
        return false;
    }

    const auto node = nodes.find(id);
    if (node == nodes.end()) {
        executed_nodes.erase(id);
        return false;
    }

    node->second.node->execute(context, nodes, node->second.state);
    return true;
}

void submit_demanding_nodes(execution_context_s*              context,
                            const node_map_t&                 nodes,
                            std::span<const std::string_view> demanding_nodes)
{
    for (const auto id : demanding_nodes) {
        submit_node_once(context, nodes, id);
    }
}

void execute_demanding_nodes(execution_context_s*              context,
                             const node_map_t&                 nodes,
                             std::span<const std::string_view> demanding_nodes)
{
    for (const auto id : demanding_nodes) {
        execute_node_once(context, nodes, id);
    }
}

} // namespace miximus::nodes
