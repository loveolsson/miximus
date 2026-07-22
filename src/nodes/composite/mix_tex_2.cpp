#include "core/app_state.hpp"
#include "glm/common.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/geometry.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <cstdint>
#include <memory>

namespace {
using namespace miximus;
using namespace miximus::nodes;

enum class blend_mode_e : uint8_t
{
    video,
    linear,
};

class node_impl : public node_i
{
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{*this, "fb_in"};
    input_interface_s<gpu::texture_s*>      iface_a_{*this, "a"};
    input_interface_s<gpu::texture_s*>      iface_b_{*this, "b"};
    input_interface_s<double>               iface_t_{*this, "t"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<gpu::textured_quad_s> textured_quad_;

  public:
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

        gpu::texture_s* a{};
        gpu::texture_s* b{};
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

        const auto target_dimensions = framebuffer->texture()->display_dimensions();
        const auto a_rect            = gpu::contain_aspect_ratio({}, a->display_dimensions(), target_dimensions);
        const auto b_rect            = gpu::contain_aspect_ratio({}, b->display_dimensions(), target_dimensions);
        const auto blend_mode        = state.get_enum_option("blend_mode", blend_mode_e::video);
        const auto mix_space         = blend_mode == blend_mode_e::video ? gpu::textured_quad_s::mix_space_e::video
                                                                         : gpu::textured_quad_s::mix_space_e::linear;

        framebuffer->begin_render();

        if (!textured_quad_) {
            auto* shader   = app->ctx()->get_shader(gpu::shader_program_s::name_e::texture_mix);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        textured_quad_->draw_mix(a, b, t, a_rect, b_rect, mix_space);
        gpu::framebuffer_s::end_render();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Mix A/B"                          },
            {"t",          0                                  },
            {"blend_mode", enum_to_string(blend_mode_e::video)},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "t") {
            return normalize_option_value<double>(value, 0, 1);
        }
        if (name == "blend_mode") {
            const auto result = normalize_option_value<std::string_view>(value);
            if (result == option_result_e::invalid) {
                return result;
            }

            return enum_from_string<blend_mode_e>(value->get<std::string_view>()).has_value()
                       ? result
                       : option_result_e::invalid;
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "mix_tex_2"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_mix_tex_2_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
