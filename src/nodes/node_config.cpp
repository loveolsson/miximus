#include "node_config.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"

namespace miximus::nodes {

node* node_cfg_t::find_node(const std::string& id) const
{
    auto it = nodes.find(id);
    if (it != nodes.end()) {
        return it->second.get();
    }

    return nullptr;
}

bool node_cfg_t::erase_node(const std::string& id)
{
    auto it = nodes.find(id);
    if (it != nodes.end()) {
        nodes.erase(it);
        return true;
    }

    return false;
}

bool node_cfg_t::erase_connection(const connection& con)
{
    auto it = connections.find(con);
    if (it != connections.end()) {
        auto& con = *it;

        if (auto from_node = find_node(con.from_node)) {
            if (auto iface = from_node->find_interface(con.from_interface)) {
                iface->remove_connection(con);
            }
        }

        if (auto to_node = find_node(con.to_node)) {
            if (auto iface = to_node->find_interface(con.to_interface)) {
                iface->remove_connection(con);
            }
        }

        connections.erase(it);
        return true;
    }

    return false;
}

} // namespace miximus::nodes