#include "core/app_state.hpp"
#include "glm/common.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/types.hpp"
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
    input_interface_s<gpu::rect_s>          iface_rect_{*this, "rect"};
    input_interface_s<gpu::texture_s*>      iface_tex_{*this, "tex"};
    input_interface_s<double>               iface_opacity_{*this, "opacity"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<gpu::textured_quad_s> textured_quad_;

  public:
    explicit node_impl() = default;

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto texture = iface_tex_.resolve_value(app, nodes, state);

        if (texture == nullptr) {
            return;
        }

        auto draw_rect = iface_rect_.resolve_value(app,
                                                   nodes,
                                                   state,
                                                   {
                                                       .pos  = {0,   0  },
                                                       .size = {1.0, 1.0},
        });

        auto opacity_opt = state.get_option<double>("opacity", 1.0);
        auto opacity     = iface_opacity_.resolve_value(app, nodes, state, opacity_opt);
        opacity          = glm::clamp(opacity, 0.0, 1.0);

        fb->begin_render();

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        textured_quad_->draw(texture, draw_rect, opacity);

        gpu::framebuffer_s::end_render();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",    "Draw box"},
            {"opacity", 1         },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "opacity") {
            return normalize_option_value<double>(value, 0, 1.0);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "draw_box"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_draw_box_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
