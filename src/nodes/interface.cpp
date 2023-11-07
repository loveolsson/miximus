#include "nodes/interface.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "nodes/node.hpp"

#include <stdexcept>

namespace miximus::nodes {

bool interface_i::add_connection(con_set_t* connections, const connection_s& con, con_set_t* removed) const
{
    if (connections->size() == max_connection_count_) {
        // This causes a remove_connection action at a later stage
        removed->emplace(removed->end(), connections->front());
    }

    connections->emplace_back(con);
    return true;
}

interface_i::resolved_cons_t
interface_i::resolve_connections(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) const
{
    resolved_cons_t res;

    if (direction() == dir_e::output) {
        throw std::runtime_error("resolve_connection called on output interface");
    }

    auto connections = state.get_connection_set(name_);
    res.reserve(connections.size());

    for (const auto& con : connections) {
        const interface_i* iface = nullptr;

        if (auto record = nodes.find(con.from_node); record != nodes.end()) {
            const auto& node  = record->second.node;
            const auto& state = record->second.state;

            iface = node->find_interface(con.from_interface);

            if (iface != nullptr && !state.executed) {
                state.executed = true;
                node->execute(app, nodes, state);
            }
        }

        res.emplace_back(iface);
    }

    return res;
}

template <>
bool input_interface_s<double>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::f64:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::vec2_t>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::f64:
        case interface_type_e::vec2:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::rect_s>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::rect:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::texture_s*>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::texture:
        case interface_type_e::framebuffer:
            return true;
        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::framebuffer_s*>::accepts(interface_type_e type) const
{
    switch (type) {
        case interface_type_e::framebuffer:
            return true;
        default:
            return false;
    }
}

template <>
double input_interface_s<double>::cast_iface_to_value(const interface_i* iface, const double& fallback)
{
    if (const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

template <>
gpu::vec2_t input_interface_s<gpu::vec2_t>::cast_iface_to_value(const interface_i* iface, const gpu::vec2_t& fallback)
{
    if (const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2_t>*>(iface)) {
        return cast->get_value();
    }

    if (const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface)) {
        auto val = cast->get_value();
        return {val, val};
    }

    return fallback;
}

template <>
gpu::rect_s input_interface_s<gpu::rect_s>::cast_iface_to_value(const interface_i* iface, const gpu::rect_s& fallback)
{
    if (const auto* cast = dynamic_cast<const output_interface_s<gpu::rect_s>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

template <>
gpu::texture_s* input_interface_s<gpu::texture_s*>::cast_iface_to_value(const interface_i*     iface,
                                                                        gpu::texture_s* const& fallback)
{
    if (const auto* cast = dynamic_cast<const output_interface_s<gpu::texture_s*>*>(iface)) {
        return cast->get_value();
    }

    if (const auto* cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface)) {
        if (auto* fb = cast->get_value()) {
            if (auto* texture = fb->texture()) {
                texture->generate_mip_maps();
                return texture;
            }
        }
    }

    return fallback;
}

template <>
gpu::framebuffer_s* input_interface_s<gpu::framebuffer_s*>::cast_iface_to_value(const interface_i*         iface,
                                                                                gpu::framebuffer_s* const& fallback)
{
    if (const auto* cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

} // namespace miximus::nodes