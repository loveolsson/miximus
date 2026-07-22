#include "nodes/interface.hpp"

#include "core/app_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "nodes/frame_execution.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"

#include <stdexcept>

namespace miximus::nodes {

interface_i::interface_i(node_i& owner, std::string_view name, dir_e direction, interface_type_e type)
    : name_(name)
    , direction_(direction)
    , type_(type)
{
    owner.register_interface(*this);
}

bool interface_i::add_connection(con_set_t* connections, const connection_s& con, con_set_t* removed) const
{
    if (connections->size() == static_cast<size_t>(max_connection_count_)) {
        // This causes a remove_connection action at a later stage
        removed->emplace(removed->end(), connections->front());
    }

    connections->emplace_back(con);
    return true;
}

std::span<const connection_s> interface_i::connections(const node_state_s& state) const
{
    if (direction() == dir_e::output) {
        throw std::logic_error("connections called on output interface");
    }
    return state.get_connection_set(name_);
}

void interface_i::submit_connections(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) const
{
    for (const auto& connection : connections(state)) {
        submit_node_once(app, nodes, connection.from_node, app->frame_info.submitted_nodes);
    }
}

interface_i::resolved_cons_t
interface_i::resolve_connections(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) const
{
    resolved_cons_t res;

    const auto connected = connections(state);
    res.reserve(connected.size());

    for (const auto& con : connected) {
        const interface_i* iface = nullptr;

        if (auto record = nodes.find(con.from_node); record != nodes.end()) {
            const auto& node = record->second.node;

            iface = node->find_interface(con.from_interface);

            if (iface != nullptr) {
                execute_node_once(app, nodes, record->first, app->frame_info.executed_nodes);
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
    if (const auto cast = dynamic_cast<const output_interface_s<double>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

template <>
gpu::vec2_t input_interface_s<gpu::vec2_t>::cast_iface_to_value(const interface_i* iface, const gpu::vec2_t& fallback)
{
    if (const auto cast = dynamic_cast<const output_interface_s<gpu::vec2_t>*>(iface)) {
        return cast->get_value();
    }

    if (const auto cast = dynamic_cast<const output_interface_s<double>*>(iface)) {
        auto val = cast->get_value();
        return {val, val};
    }

    return fallback;
}

template <>
gpu::rect_s input_interface_s<gpu::rect_s>::cast_iface_to_value(const interface_i* iface, const gpu::rect_s& fallback)
{
    if (const auto cast = dynamic_cast<const output_interface_s<gpu::rect_s>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

template <>
gpu::texture_s* input_interface_s<gpu::texture_s*>::cast_iface_to_value(const interface_i*     iface,
                                                                        gpu::texture_s* const& fallback)
{
    if (const auto cast = dynamic_cast<const output_interface_s<gpu::texture_s*>*>(iface)) {
        auto* texture = cast->get_value();
        return texture != nullptr ? texture : fallback;
    }

    if (const auto cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface)) {
        if (auto fb = cast->get_value()) {
            if (auto texture = fb->texture()) {
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
    if (const auto cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface)) {
        return cast->get_value();
    }

    return fallback;
}

} // namespace miximus::nodes
