#include "core/app_state.hpp"
#include "glm/common.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/geometry.hpp"
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
    input_interface_s<gpu::texture_s*>      iface_tex_{*this, "tex"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<gpu::textured_quad_s> textured_quad_;

  public:
    explicit node_impl() { iface_tex_.set_max_connection_count(INT_MAX); }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto textures = iface_tex_.resolve_values<9>(app, nodes, state);

        if (textures.empty()) {
            return;
        }

        const size_t tex_count = textures.size();
        size_t       cols      = 1;
        for (; (cols * cols) < tex_count; cols++) {
        }

        const double box_dim = 1.0 / static_cast<double>(cols);

        fb->begin_render();

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        const auto target_dimensions = fb->texture()->display_dimensions();
        auto       batch             = textured_quad_->begin_batch();

        for (size_t i = 0, y = 0; y < cols && i < tex_count; y++) {
            for (size_t x = 0; x < cols && i < tex_count; x++, i++) {
                const auto texture = textures[i];
                if (texture == nullptr) {
                    continue;
                }

                const gpu::rect_s cell{
                    .pos  = {box_dim * static_cast<double>(x), box_dim * static_cast<double>(y)},
                    .size = {box_dim,                          box_dim                         },
                };
                const auto draw_rect =
                    gpu::contain_aspect_ratio(cell, texture->display_dimensions(), target_dimensions);

                batch.draw(texture, draw_rect);
            }
        }
        gpu::framebuffer_s::end_render();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Infinite Multiviewer"},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        (void)name;
        (void)value;
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "infinite_multiviewer"; }
};

} // namespace

namespace miximus::nodes::composite {

std::shared_ptr<node_i> create_infinite_multiviewer_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::composite
