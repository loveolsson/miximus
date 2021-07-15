#include "nodes/interface.hpp"
#include "nodes/node.hpp"

#include <stdexcept>

namespace miximus::nodes {

bool interface_i::add_connection(con_set_t* connections, const connection& con, con_set_t& removed) const
{
    if (direction_ == dir::input && !connections->empty()) {
        removed.emplace(*connections->begin());
    }

    auto [_, success] = connections->emplace(con);
    return success;
}

interface_i* interface_i::resolve_connection(const node_map_t& nodes, const con_set_t& connections)
{
    if (direction_ == dir::output) {
        throw std::runtime_error("resolve_connection called on output interface");
    }

    if (!connections.empty()) {
        const connection& con    = *connections.begin();
        auto              record = nodes.find(con.from_node);
        if (record != nodes.end()) {
            const auto& node  = record->second.node;
            auto&       state = record->second.state;
            return node->get_prepared_interface(nodes, state, con.from_interface);
        }
    }

    return nullptr;
}

template <>
bool interface<double>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::f64:
        case interface_type_e::i64:
            return true;

        default:
            return false;
    }
}

template <>
bool interface<int64_t>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::f64:
        case interface_type_e::i64:
            return true;

        default:
            return false;
    }
}

template <>
bool interface<gpu::vec2>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::f64:
        case interface_type_e::i64:
        case interface_type_e::vec2:
            return true;

        default:
            return false;
    }
}

template <>
bool interface<double>::resolve_connection_value(const node_map_t& nodes, const con_set_t& connections)
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                value_ = dynamic_cast<interface<double>*>(iface)->get_value();
                return true;
            }

            case interface_type_e::i64: {
                value_ = static_cast<double>(dynamic_cast<interface<int64_t>*>(iface)->get_value());
                return true;
            }

            default:
                break;
        }
    }

    value_ = double{};
    return false;
}

template <>
bool interface<int64_t>::resolve_connection_value(const node_map_t& nodes, const con_set_t& connections)
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                value_ = static_cast<int64_t>(dynamic_cast<interface<double>*>(iface)->get_value());
                return true;
            }

            case interface_type_e::i64: {
                value_ = dynamic_cast<interface<int64_t>*>(iface)->get_value();
                return true;
            }

            default:
                break;
        }
    }

    value_ = int64_t{};
    return false;
}

template <>
bool interface<gpu::vec2>::resolve_connection_value(const node_map_t& nodes, const con_set_t& connections)
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                auto val = dynamic_cast<interface<double>*>(iface)->get_value();
                value_   = {val, val};
                return true;
            }

            case interface_type_e::i64: {
                auto val = dynamic_cast<interface<int64_t>*>(iface)->get_value();
                value_   = {val, val};
                return true;
            }

            case interface_type_e::vec2: {
                value_ = dynamic_cast<interface<gpu::vec2>*>(iface)->get_value();
                return true;
            }

            default:
                break;
        }
        return true;
    }

    value_ = gpu::vec2(0, 0);
    return false;
}

} // namespace miximus::nodes