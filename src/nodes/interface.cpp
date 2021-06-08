#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_config.hpp"

#include <stdexcept>

namespace miximus::nodes {

bool interface::add_connection(const connection& con, std::vector<connection>& removed)
{
    if (single_connection_ && !connections_.empty()) {
        removed.push_back(*connections_.begin());
    }

    auto [_, success] = connections_.emplace(con);
    return success;
}

bool interface::remove_connection(const connection& con)
{
    size_t removed = connections_.erase(con);
    return (removed > 0);
}

std::vector<interface*> interface::resolve_connections(const node_cfg_t& cfg)
{
    std::vector<interface*> result;

    for (auto& con : connections_) {
        if (auto n = cfg.find_node(con.from_node)) {
            if (auto iface = n->get_prepared_interface(cfg, con.from_interface)) {
                result.emplace_back(iface);
            }
        }
    }

    return result;
}

/**
 * Interface converters
 */

template <>
double interface_typed<double, false>::get_value_from(interface* iface)
{
    switch (iface->type()) {
        case interface_type_e::f64: {
            auto typed = static_cast<interface_typed<double, false>*>(iface);
            return typed->get_value();
        }
        case interface_type_e::i64: {
            auto typed = static_cast<interface_typed<int64_t, false>*>(iface);
            return typed->get_value();
        }

        default:
            throw std::runtime_error("incompatible interface types");
            return 0;
    }
}

template <>
int64_t interface_typed<int64_t, false>::get_value_from(interface* iface)
{
    switch (iface->type()) {
        case interface_type_e::f64: {
            auto typed = static_cast<interface_typed<double, false>*>(iface);
            return typed->get_value();
        }
        case interface_type_e::i64: {
            auto typed = static_cast<interface_typed<int64_t, false>*>(iface);
            return typed->get_value();
        }

        default:
            throw std::runtime_error("incompatible interface types");
            return 0;
    }
}

} // namespace miximus::nodes