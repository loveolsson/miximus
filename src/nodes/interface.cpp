#include "nodes/interface.hpp"
#include "nodes/node.hpp"

#include <stdexcept>

namespace miximus::nodes {

template <>
interface_type_e interface_i::get_interface_type<double>()
{
    return interface_type_e::f64;
}

template <>
interface_type_e interface_i::get_interface_type<int64_t>()
{
    return interface_type_e::i64;
}

template <>
interface_type_e interface_i::get_interface_type<gpu::vec2>()
{
    return interface_type_e::vec2;
}

bool interface_i::add_connection(con_set_t* connections, const connection_s& con, con_set_t& removed) const
{
    if (direction() == dir_e::input && !connections->empty()) {
        removed.emplace(*connections->begin());
    }

    auto [_, success] = connections->emplace(con);
    return success;
}

const interface_i*
interface_i::resolve_connection(core::app_state_s& app, const node_map_t& nodes, const con_set_t& connections) const
{
    if (direction() == dir_e::output) {
        throw std::runtime_error("resolve_connection called on output interface");
    }

    if (!connections.empty()) {
        const connection_s& con    = *connections.begin();
        auto                record = nodes.find(con.from_node);
        if (record != nodes.end()) {
            const auto& node  = record->second.node;
            const auto& state = record->second.state;

            const auto* iface = node->find_interface(con.from_interface);
            if (iface == nullptr) {
                return nullptr;
            }

            if (!state.executed) {
                state.executed = true;
                node->execute(app, nodes, state);
            }

            return iface;
        }
    }

    return nullptr;
}

template <>
bool input_interface_s<double>::accepts(interface_type_e type) const
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
bool input_interface_s<int64_t>::accepts(interface_type_e type) const
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
bool input_interface_s<gpu::vec2>::accepts(interface_type_e type) const
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
double input_interface_s<double>::resolve_value(core::app_state_s& app,
                                                const node_map_t&  nodes,
                                                const con_set_t&   connections,
                                                const double&      fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case interface_type_e::i64: {
                const auto* cast = dynamic_cast<const output_interface_s<int64_t>*>(iface);
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

    return fallback;
}

template <>
int64_t input_interface_s<int64_t>::resolve_value(core::app_state_s& app,
                                                  const node_map_t&  nodes,
                                                  const con_set_t&   connections,
                                                  const int64_t&     fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return static_cast<int64_t>(cast->get_value());
            }

            case interface_type_e::i64: {
                const auto* cast = dynamic_cast<const output_interface_s<int64_t>*>(iface);
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

    return fallback;
}

template <>
gpu::vec2 input_interface_s<gpu::vec2>::resolve_value(core::app_state_s& app,
                                                      const node_map_t&  nodes,
                                                      const con_set_t&   connections,
                                                      const gpu::vec2&   fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case interface_type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return gpu::vec2{val, val};
            }

            case interface_type_e::i64: {
                const auto* cast = dynamic_cast<const output_interface_s<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return gpu::vec2{val, val};
            }

            case interface_type_e::vec2: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2>*>(iface);
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

    return fallback;
}

} // namespace miximus::nodes