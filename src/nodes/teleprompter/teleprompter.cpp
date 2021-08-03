#include "teleprompter.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"

#include <glm/glm.hpp>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_;
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_;

    std::unique_ptr<gpu::draw_state_s> draw_state_;
    std::unique_ptr<render::surface_s> surface_;
    render::font_loader_s              font_loader_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("fb_in", &iface_fb_in_);
        interfaces_.emplace("fb_out", &iface_fb_out_);
    }

    void prepare(core::app_state_s* app, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto text = state.get_option<std::string_view>("text");

        auto* fb = iface_fb_in_.resolve_value(app, nodes, state.get_connection_set("fb_in"));
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
        }

        if (!surface_) {
            auto dim = fb->get_texture()->texture_dimensions();
            surface_ = std::make_unique<render::surface_s>(dim);
            surface_->clear({100, 0, 0, 0});

            const auto* font = app->font_registry()->find_font_variant("Ubuntu", "Regular");
            if (font == nullptr) {
                return;
            }

            auto instance = font_loader_.load_font(font);
            if (!instance) {
                return;
            }

            size_t height = 60;

            instance->set_size(height);

            std::u32string_view str(
                U"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Donec dapibus aliquam metus quis placerat. "
                U"Pellentesque quis lacus odio. Phasellus fermentum, sapien eu congue luctus, mauris lorem suscipit "
                U"mauris, a commodo magna tortor vitae metus. Fusce tincidunt facilisis hendrerit. Nam vel arcu et "
                U"nulla eleifend aliquet. Cras id lobortis metus. Sed vitae aliquet purus. Sed commodo velit non "
                U"suscipit gravida.");
            std::vector<std::u32string_view> lines;
            size_t                           pos = 0;

            while (pos < str.size()) {
                size_t consumed = instance->fit_line(str.substr(pos), dim.x - 100);
                lines.emplace_back(str.substr(pos, consumed));
                pos += consumed;
            }

            float y = height;
            for (auto& line : lines) {
                instance->draw_line(line, surface_.get(), {50, y});
                y += height + height / 2;
            }

            surface_->transfer()->perform_copy();
            surface_->transfer()->wait_for_copy();
            surface_->transfer()->perform_transfer(surface_->texture());
        }

        fb->bind();

        auto* texture = surface_->texture();
        texture->bind(0);
        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", gpu::vec2_t{0, 1});
        shader->set_uniform("scale", gpu::vec2_t{1, -1});

        draw_state_->draw();

        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::unbind();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Teleprompter"},
            {"size", nlohmann::json::array({1280, 720})},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "text") {
            return value.is_string();
        }

        return false;
    }

    std::string_view type() const final { return "teleprompter"; }
};

} // namespace

namespace miximus::nodes::teleprompter {

std::shared_ptr<node_i> create_teleprompter_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::teleprompter
