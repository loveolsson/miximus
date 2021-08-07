#include "nodes/interface.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "nodes/node.hpp"

#include <stdexcept>

namespace miximus::nodes {

template <>
interface_i::type_e interface_i::get_interface_type<double>()
{
    return type_e::f64;
}

template <>
interface_i::type_e interface_i::get_interface_type<int64_t>()
{
    return type_e::i64;
}

template <>
interface_i::type_e interface_i::get_interface_type<gpu::vec2_t>()
{
    return type_e::vec2;
}

template <>
interface_i::type_e interface_i::get_interface_type<gpu::vec2i_t>()
{
    return type_e::vec2i;
}

template <>
interface_i::type_e interface_i::get_interface_type<gpu::texture_s*>()
{
    return type_e::texture;
}

template <>
interface_i::type_e interface_i::get_interface_type<gpu::framebuffer_s*>()
{
    return type_e::framebuffer;
}

bool interface_i::add_connection(con_set_t* connections, const connection_s& con, con_set_t& removed) const
{
    if (connections->size() == max_connection_count_) {
        removed.emplace(*connections->begin());
    }

    auto [_, success] = connections->emplace(con);
    return success;
}

const interface_i*
interface_i::resolve_connection(core::app_state_s* app, const node_map_t& nodes, const con_set_t& connections) const
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
bool input_interface_s<double>::accepts(type_e type) const
{
    switch (type) {
        case type_e::f64:
        case type_e::i64:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<int64_t>::accepts(type_e type) const
{
    switch (type) {
        case type_e::f64:
        case type_e::i64:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::vec2_t>::accepts(type_e type) const
{
    switch (type) {
        case type_e::f64:
        case type_e::i64:
        case type_e::vec2:
        case type_e::vec2i:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::vec2i_t>::accepts(type_e type) const
{
    switch (type) {
        case type_e::f64:
        case type_e::i64:
        case type_e::vec2:
        case type_e::vec2i:
            return true;

        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::texture_s*>::accepts(type_e type) const
{
    switch (type) {
        case type_e::texture:
        case type_e::framebuffer:
            return true;
        default:
            return false;
    }
}

template <>
bool input_interface_s<gpu::framebuffer_s*>::accepts(type_e type) const
{
    switch (type) {
        case type_e::framebuffer:
            return true;
        default:
            return false;
    }
}

template <>
double input_interface_s<double>::resolve_value(core::app_state_s* app,
                                                const node_map_t&  nodes,
                                                const con_set_t&   connections,
                                                double             fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case type_e::i64: {
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
int64_t input_interface_s<int64_t>::resolve_value(core::app_state_s* app,
                                                  const node_map_t&  nodes,
                                                  const con_set_t&   connections,
                                                  int64_t            fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return static_cast<int64_t>(cast->get_value());
            }

            case type_e::i64: {
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
gpu::vec2_t input_interface_s<gpu::vec2_t>::resolve_value(core::app_state_s* app,
                                                          const node_map_t&  nodes,
                                                          const con_set_t&   connections,
                                                          gpu::vec2_t        fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return {val, val};
            }

            case type_e::i64: {
                const auto* cast = dynamic_cast<const output_interface_s<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return {val, val};
            }

            case type_e::vec2: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case type_e::vec2i: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2i_t>*>(iface);
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
gpu::vec2i_t input_interface_s<gpu::vec2i_t>::resolve_value(core::app_state_s* app,
                                                            const node_map_t&  nodes,
                                                            const con_set_t&   connections,
                                                            gpu::vec2i_t       fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::f64: {
                const auto* cast = dynamic_cast<const output_interface_s<double>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return {val, val};
            }

            case type_e::i64: {
                const auto* cast = dynamic_cast<const output_interface_s<int64_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto val = cast->get_value();
                return {val, val};
            }

            case type_e::vec2: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2_t>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case type_e::vec2i: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::vec2i_t>*>(iface);
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
gpu::texture_s* input_interface_s<gpu::texture_s*>::resolve_value(core::app_state_s* app,
                                                                  const node_map_t&  nodes,
                                                                  const con_set_t&   connections,
                                                                  gpu::texture_s*    fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::texture: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::texture_s*>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                return cast->get_value();
            }

            case type_e::framebuffer: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface);
                if (cast == nullptr) {
                    assert(false);
                    break;
                }
                auto* fb = cast->get_value();
                if (fb == nullptr) {
                    break;
                }

                auto* texture = fb->texture();
                if (texture != nullptr) {
                    texture->generate_mip_maps();
                }

                return texture;
            }

            default:
                break;
        }
    }

    return fallback;
}

template <>
gpu::framebuffer_s* input_interface_s<gpu::framebuffer_s*>::resolve_value(core::app_state_s*  app,
                                                                          const node_map_t&   nodes,
                                                                          const con_set_t&    connections,
                                                                          gpu::framebuffer_s* fallback) const
{
    if (const auto* iface = resolve_connection(app, nodes, connections)) {
        switch (iface->type()) {
            case type_e::framebuffer: {
                const auto* cast = dynamic_cast<const output_interface_s<gpu::framebuffer_s*>*>(iface);
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