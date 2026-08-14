#include "nodes/interface.hpp"

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

void interface_i::add_connection(con_set_t* connections, const connection_s& con, con_set_t* removed) const
{
    if (connections->size() == max_connection_count_) {
        // This causes a remove_connection action at a later stage
        removed->emplace(removed->end(), connections->front());
    }

    connections->emplace_back(con);
}

std::span<const connection_s> interface_i::connections(const node_state_s& state) const
{
    if (direction() == dir_e::output) {
        throw std::logic_error("connections called on output interface");
    }
    return state.get_connection_set(name_);
}

void interface_i::submit_dependencies(core::app_state_s*            app,
                                      const node_map_t&             nodes,
                                      std::span<const connection_s> connected)
{
    for (const auto& connection : connected) {
        submit_node_once(app, nodes, connection.from_node);
    }
}

const interface_i*
interface_i::resolve_connection(core::app_state_s* app, const node_map_t& nodes, const connection_s& connection)
{
    const auto record = nodes.find(connection.from_node);
    if (record == nodes.end()) {
        return nullptr;
    }

    const auto* iface = record->second.node->find_interface(connection.from_interface);
    if (iface != nullptr) {
        execute_node_once(app, nodes, record->first);
    }
    return iface;
}

interface_i::resolved_cons_t interface_i::resolve_connections(core::app_state_s*            app,
                                                              const node_map_t&             nodes,
                                                              std::span<const connection_s> connected)
{
    resolved_cons_t res;
    res.reserve(connected.size());

    for (const auto& con : connected) {
        res.emplace_back(resolve_connection(app, nodes, con));
    }

    return res;
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
    if (iface == nullptr) {
        return fallback;
    }

    switch (iface->type()) {
        case interface_type_e::vec2: {
            const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2_t>*>(iface);
            return cast != nullptr ? cast->get_value() : fallback;
        }
        case interface_type_e::f64: {
            const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
            if (cast == nullptr) {
                return fallback;
            }
            const auto value = cast->get_value();
            return {value, value};
        }
        default:
            return fallback;
    }
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
    if (iface == nullptr) {
        return fallback;
    }

    switch (iface->type()) {
        case interface_type_e::texture: {
            const auto* cast = dynamic_cast<const output_interface_s<gpu::texture_s*>*>(iface);
            if (cast == nullptr) {
                return fallback;
            }
            auto* texture = cast->get_value();
            return texture != nullptr ? texture : fallback;
        }
        case interface_type_e::framebuffer: {
            const auto* cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface);
            if (cast == nullptr) {
                return fallback;
            }
            auto* framebuffer = cast->get_value();
            auto* texture     = framebuffer != nullptr ? framebuffer->texture() : nullptr;
            if (texture != nullptr) {
                texture->generate_mip_maps();
                return texture;
            }
            return fallback;
        }
        default:
            return fallback;
    }
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
