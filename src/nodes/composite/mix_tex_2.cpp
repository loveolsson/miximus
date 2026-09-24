#include "core/app_state.hpp"
#include "glm/common.hpp"
#include "gpu/drawing.hpp"
#include "gpu/geometry.hpp"
#include "gpu/texture.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <memory>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::texture_s*>       iface_fb_in_{*this, "fb_in"};
    input_interface_s<const gpu::texture_s*> iface_a_{*this, "a"};
    input_interface_s<const gpu::texture_s*> iface_b_{*this, "b"};
    input_interface_s<double>                iface_t_{*this, "t"};
    output_interface_s<gpu::texture_s*>      iface_fb_out_{*this, "fb_out"};

  public:
    void submit(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        interface_i::submit_dependencies(app, nodes, iface_fb_in_.connections(state));

        const auto t_connections = iface_t_.connections(state);
        interface_i::submit_dependencies(app, nodes, t_connections);

        if (!t_connections.empty()) {
            interface_i::submit_dependencies(app, nodes, iface_a_.connections(state));
            interface_i::submit_dependencies(app, nodes, iface_b_.connections(state));
            return;
        }

        const auto t = state.get_option<double>("t");
        if (t < 1.0) {
            interface_i::submit_dependencies(app, nodes, iface_a_.connections(state));
        }
        if (t > 0.0) {
            interface_i::submit_dependencies(app, nodes, iface_b_.connections(state));
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto* framebuffer = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(framebuffer);

        if (framebuffer == nullptr) {
            return;
        }

        const auto t_option = state.get_option<double>("t");
        const auto t_value  = iface_t_.resolve_value(app, nodes, state, t_option);
        const auto t        = glm::clamp(t_value, 0.0, 1.0);
        auto*      fallback = app->fallback_texture();

        const gpu::texture_s* a{};
        const gpu::texture_s* b{};
        if (t <= 0.0) {
            a = iface_a_.resolve_value(app, nodes, state, fallback);
            b = a;
        } else if (t >= 1.0) {
            b = iface_b_.resolve_value(app, nodes, state, fallback);
            a = b;
        } else {
            a = iface_a_.resolve_value(app, nodes, state, fallback);
            b = iface_b_.resolve_value(app, nodes, state, fallback);
        }

        const auto target_dimensions = framebuffer->dimensions();
        const auto fill_mode         = state.get_enum_option_unchecked<gpu::fill_mode_e>("fill_mode");

        const auto a_draw     = gpu::calculate_texture_draw({}, a->dimensions(), target_dimensions, fill_mode);
        const auto b_draw     = gpu::calculate_texture_draw({}, b->dimensions(), target_dimensions, fill_mode);
        const auto blend_mode = state.get_enum_option_unchecked<gpu::blend_mode_e>("blend_mode");

        gpu::mix_textures(app->commands(), a, b, framebuffer, t, a_draw, b_draw, blend_mode);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Mix A/B"                                },
            {"t",          0                                        },
            {"blend_mode", enum_to_string(gpu::blend_mode_e::video) },
            {"fill_mode",  enum_to_string(gpu::fill_mode_e::contain)},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "t") {
            return normalize_option_value<double>(value, 0, 1);
        }
        if (name == "blend_mode") {
            return normalize_enum_option_value<gpu::blend_mode_e>(value);
        }
        if (name == "fill_mode") {
            return normalize_enum_option_value<gpu::fill_mode_e>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "mix_tex_2"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_mix_tex_2_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
