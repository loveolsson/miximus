#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_config.hpp"

#include <stdexcept>

namespace miximus::nodes {

bool interface::add_connection(const connection& con, con_set_t& removed)
{
    if (max_connection_count_ == 1 && !connections_.empty()) {
        removed.emplace(*connections_.begin());
    } else if (connections_.size() >= max_connection_count_) {
        return false;
    }

    auto [_, success] = connections_.emplace(con);
    return success;
}

bool interface::remove_connection(const connection& con)
{
    size_t removed = connections_.erase(con);
    return (removed > 0);
}

interface* interface::resolve_connection(const node_cfg& cfg)
{
    if (!is_input_) {
        throw std::runtime_error("resolve_connection called on output interface");
    }

    if (!connections_.empty()) {
        const connection& con = *connections_.begin();
        if (node* n = cfg.find_node(con.from_node)) {
            return n->get_prepared_interface(cfg, con.from_interface);
        }
    }

    return nullptr;
}

/**
 * Interface converters
 */

template <>
double interface_typed<double>::get_value_from(interface* iface)
{
    auto res = double{};

    switch (iface->type()) {
        case interface_type_e::f64:
            if (auto* dyn = dynamic_cast<interface_typed<double>*>(iface)) {
                res = dyn->get_value();
            }
            break;

        case interface_type_e::i64:
            if (auto* dyn = dynamic_cast<interface_typed<int64_t>*>(iface)) {
                res = dyn->get_value();
            }
            break;

        default:
            throw std::runtime_error("incompatible interface types");
    }

    return res;
}

template <>
int64_t interface_typed<int64_t>::get_value_from(interface* iface)
{
    auto res = int16_t{};

    switch (iface->type()) {
        case interface_type_e::f64:
            if (auto* dyn = dynamic_cast<interface_typed<double>*>(iface)) {
                res = dyn->get_value();
            };
            break;

        case interface_type_e::i64:
            if (auto* dyn = dynamic_cast<interface_typed<int64_t>*>(iface)) {
                res = dyn->get_value();
            }
            break;

        default:
            throw std::runtime_error("incompatible interface types");
    }

    return res;
}

} // namespace miximus::nodes