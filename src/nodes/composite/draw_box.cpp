#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"

#include "glm/common.hpp"

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::rect_s>          iface_rect_{"rect"};
    input_interface_s<gpu::texture_s*>      iface_tex_{"tex"};
    input_interface_s<double>               iface_opacity_{"opacity"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{"fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{"fb_out"};

    std::unique_ptr<gpu::draw_state_s> draw_state_;

  public:
    explicit node_impl()
    {
        iface_rect_.register_interface(&interfaces_);
        iface_tex_.register_interface(&interfaces_);
        iface_opacity_.register_interface(&interfaces_);
        iface_fb_in_.register_interface(&interfaces_);
        iface_fb_out_.register_interface(&interfaces_);
    }

    void prepare(core::app_state_s* app, const node_state_s& /*nodes*/, traits_s* /*traits*/) final
    {
        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto* fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto* texture = iface_tex_.resolve_value(app, nodes, state);

        if (texture == nullptr) {
            return;
        }

        auto draw_rect = iface_rect_.resolve_value(app, nodes, state, {{0, 0}, {1.0, 1.0}});

        auto opacity_opt = state.get_option<double>("opacity", 1.0);
        auto opacity     = iface_opacity_.resolve_value(app, nodes, state, opacity_opt);
        opacity          = glm::clamp(opacity, 0.0, 1.0);

        fb->bind();

        auto fb_dim = fb->texture()->texture_dimensions();
        glViewport(0, 0, fb_dim.x, fb_dim.y);

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", draw_rect.pos);
        shader->set_uniform("scale", draw_rect.size);
        shader->set_uniform("opacity", opacity);

        texture->bind(0);
        draw_state_->draw();
        gpu::texture_s::unbind(0);

        gpu::framebuffer_s::unbind();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Draw box"},
            {"opacity", 1},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "opacity") {
            return validate_option<double>(value, 0, 1.0);
        }

        return false;
    }

    std::string_view type() const final { return "draw_box"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
