#include "nodes/node_config.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"

#include <set>
#include <string_view>

namespace miximus::nodes {

node* node_cfg::find_node(const std::string& id) const
{
    auto it = nodes.find(id);
    if (it != nodes.end()) {
        return it->second.get();
    }

    return nullptr;
}

bool node_cfg::erase_node(const std::string& id)
{
    auto it = nodes.find(id);
    if (it != nodes.end()) {
        nodes.erase(it);
        return true;
    }

    return false;
}

bool node_cfg::remove_connection(const connection& con)
{
    auto remove_from_interface = [&](auto& node_name, auto& iface_name) {
        if (auto n = find_node(node_name)) {
            if (auto iface = n->find_interface(iface_name)) {
                return iface->remove_connection(con);
            }
        }

        return false;
    };

    remove_from_interface(con.from_node, con.from_interface);
    remove_from_interface(con.to_node, con.to_interface);
    return connections.erase(con) > 0;
}

static bool is_connection_circular_helper(const node_cfg&             cfg,
                                          std::set<std::string_view>* cleared_nodes,
                                          std::string_view            target_node_id,
                                          const connection&           con)
{
    if (cleared_nodes->count(con.from_node) > 0) {
        return false;
    }

    if (con.from_node == target_node_id) {
        spdlog::get("app")->warn("Connection to {} rejected because link between {} and {} makes connection circular",
                                 target_node_id,
                                 con.from_node,
                                 con.to_node);
        return true;
    }

    if (const auto* node_ = cfg.find_node(con.from_node)) {
        for (const auto [_, iface] : node_->get_interfaces()) {
            if (!iface->is_input()) {
                continue;
            }

            for (const auto& c : iface->get_connections()) {
                if (is_connection_circular_helper(cfg, cleared_nodes, target_node_id, c)) {
                    return true;
                }
            }
        }
    }

    cleared_nodes->emplace(con.from_node);
    return false;
}

bool node_cfg::is_connection_circular(const connection& con) const
{
    std::set<std::string_view> cleared_nodes;
    return is_connection_circular_helper(*this, &cleared_nodes, con.to_node, con);
}

} // namespace miximus::nodes