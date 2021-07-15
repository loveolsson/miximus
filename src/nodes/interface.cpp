#include "nodes/interface.hpp"
#include "nodes/node.hpp"

#include <stdexcept>

namespace miximus::nodes {

bool interface_i::add_connection(con_set_t* connections, const connection& con, con_set_t& removed) const
{
    if (direction() == dir::input && !connections->empty()) {
        removed.emplace(*connections->begin());
    }

    auto [_, success] = connections->emplace(con);
    return success;
}

interface_i* interface_i::resolve_connection(node_map_t& nodes, const con_set_t& connections) const
{
    if (direction() == dir::output) {
        throw std::runtime_error("resolve_connection called on output interface");
    }

    if (!connections.empty()) {
        const connection& con    = *connections.begin();
        auto              record = nodes.find(con.from_node);
        if (record != nodes.end()) {
            const auto& node  = record->second.node;
            auto&       state = record->second.state;

            auto iface = node->find_interface(con.from_interface);
            if (iface == nullptr) {
                return nullptr;
            }

            if (!state.executed) {
                state.executed = true;
                node->execute(nodes, state);
            }

            return iface;
        }
    }

    return nullptr;
}

template <>
bool input_interface<double>::accepts(interface_type_e type) const
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
bool input_interface<int64_t>::accepts(interface_type_e type) const
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
bool input_interface<gpu::vec2>::accepts(interface_type_e type) const
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
double input_interface<double>::resolve_value(node_map_t& nodes, const con_set_t& connections) const
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                auto cast = dynamic_cast<output_interface<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case interface_type_e::i64: {
                auto cast = dynamic_cast<output_interface<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return static_cast<double>(cast->get_value());
            }

            default:
                break;
        }
    }

    return 0;
}

template <>
int64_t input_interface<int64_t>::resolve_value(node_map_t& nodes, const con_set_t& connections) const
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                auto cast = dynamic_cast<output_interface<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return static_cast<int64_t>(cast->get_value());
            }

            case interface_type_e::i64: {
                auto cast = dynamic_cast<output_interface<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            default:
                break;
        }
    }

    return 0;
}

template <>
gpu::vec2 input_interface<gpu::vec2>::resolve_value(node_map_t& nodes, const con_set_t& connections) const
{
    if (auto* iface = resolve_connection(nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                auto cast = dynamic_cast<output_interface<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return gpu::vec2{val, val};
            }

            case interface_type_e::i64: {
                auto cast = dynamic_cast<output_interface<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return gpu::vec2{val, val};
            }

            case interface_type_e::vec2: {
                auto cast = dynamic_cast<output_interface<gpu::vec2>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            default:
                break;
        }
    }

    return gpu::vec2{0, 0};
}

} // namespace miximus::nodes