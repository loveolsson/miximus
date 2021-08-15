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
    input_interface_s<gpu::texture_s*>      iface_tex_{"tex"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{"fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{"fb_out"};

    std::unique_ptr<gpu::draw_state_s> draw_state_;

  public:
    explicit node_impl()
    {
        iface_tex_.set_max_connection_count(INT_MAX);

        iface_tex_.register_interface(&interfaces_);
        iface_fb_in_.register_interface(&interfaces_);
        iface_fb_out_.register_interface(&interfaces_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto* fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto textures = iface_tex_.resolve_values(app, nodes, state);

        if (textures.empty()) {
            return;
        }

        int tex_count = textures.size();
        int cols      = 1;
        for (; cols * cols < tex_count; cols++) {
        }

        double box_dim = 1.0 / cols;

        fb->bind();

        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("scale", gpu::vec2_t{box_dim});
        shader->set_uniform("opacity", 1.0);

        for (int i = 0, y = 0; y < cols && i < tex_count; y++) {
            for (int x = 0; x < cols && i < tex_count; x++, i++) {
                const auto* texture = textures[i];
                if (texture == nullptr) {
                    continue;
                }

                gpu::vec2_t pos{box_dim * x, box_dim * y};
                shader->set_uniform("offset", pos);
                texture->bind(0);
                draw_state_->draw();
            }
        }

        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::unbind();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Infinite Multiviewer"},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final { return false; }

    std::string_view type() const final { return "infinite_multiviewer"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_infinite_multiviewer_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
