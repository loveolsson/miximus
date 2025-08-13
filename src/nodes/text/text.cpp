#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"

#include <locale>
#include <codecvt>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;
using namespace boost::fibers;

std::u32string decode_utf8(const std::string& utf8_string)
{
    struct destructible_codecvt : public std::codecvt<char32_t, char, std::mbstate_t>
    {
    };

    std::wstring_convert<destructible_codecvt, char32_t> utf32_converter;
    return utf32_converter.from_bytes(utf8_string);
}

class node_impl : public node_i
{
    struct text_render_info_s
    {
        std::mutex                         mtx;
        std::unique_ptr<render::surface_s> surface;
        bool                               needs_update{true};
        std::string                        last_text;
        std::string                        last_font_name;
        std::string                        last_font_variant;
        int                                last_font_size{48};
    };

    input_interface_s<gpu::vec2_t>          iface_position_in_{"position"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{"fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{"fb_out"};

    std::unique_ptr<gpu::draw_state_s>        draw_state_;
    std::shared_ptr<render::font_loader_s>    font_loader_;
    std::unique_ptr<text_render_info_s>       text_info_;
    ::mutex                                   ctx_mtx_;
    ::mutex                                   font_mtx_;
    std::unique_ptr<gpu::context_s>           ctx_;
    std::unique_ptr<render::font_instance_s>  font_instance_;

  public:
    explicit node_impl()
    {
        register_interface(&iface_position_in_);
        register_interface(&iface_fb_in_);
        register_interface(&iface_fb_out_);
        
        text_info_ = std::make_unique<text_render_info_s>();
        font_loader_ = std::make_shared<render::font_loader_s>();
    }

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        if (!ctx_) {
            ctx_ = gpu::context_s::create_unique_context(false, app->ctx());
        }

        if (!draw_state_) {
            draw_state_ = std::make_unique<gpu::draw_state_s>();
            auto shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        // Check if text or font settings have changed
        auto text = state.get_option<std::string_view>("text");
        auto font_name = state.get_option<std::string_view>("font_name");
        auto font_variant = state.get_option<std::string_view>("font_variant");
        auto font_size = state.get_option<int>("font_size");

        if (text != text_info_->last_text || 
            font_name != text_info_->last_font_name || 
            font_variant != text_info_->last_font_variant || 
            font_size != text_info_->last_font_size) {
            
            text_info_->last_text = std::string(text);
            text_info_->last_font_name = std::string(font_name);
            text_info_->last_font_variant = std::string(font_variant);
            text_info_->last_font_size = font_size;
            text_info_->needs_update = true;
        }

        if (text_info_->needs_update && !text_info_->last_text.empty()) {
            render_text(app, state);
        }
    }

    void render_text(core::app_state_s* app, const node_state_s& state)
    {
        std::unique_lock<::mutex> lock(ctx_mtx_);
        ctx_->make_current();

        // Load font if needed
        {
            std::unique_lock<::mutex> font_lock(font_mtx_);
            
            const render::font_variant_s* font_info = nullptr;
            
            spdlog::get("app")->info("Text rendering: '{}' with font '{}' size {}", 
                        text_info_->last_text, text_info_->last_font_name, text_info_->last_font_size);
            
            if (!text_info_->last_font_name.empty()) {
                font_info = app->font_registry()->find_font_variant(
                    text_info_->last_font_name, 
                    text_info_->last_font_variant
                );
            }
            
            if (font_info == nullptr) {
                // Fallback to Arial Regular
                font_info = app->font_registry()->find_font_variant("Arial", "Regular");
            }
            
            if (font_info == nullptr) {
                // Try any available font
                auto font_names = app->font_registry()->get_font_names();
                if (!font_names.empty()) {
                    font_info = app->font_registry()->find_font_variant(
                        std::string(font_names[0]), "Regular"
                    );
                }
            }
            
            if (font_info == nullptr) {
                gpu::context_s::rewind_current();
                return;
            }

            // Load font instance
            font_instance_ = font_loader_->load_font(font_info);
            if (!font_instance_) {
                gpu::context_s::rewind_current();
                return;
            }
            
            // Set the font size
            font_instance_->set_size(text_info_->last_font_size);
        }

        // Convert text to UTF-32
        auto utf32_text = decode_utf8(text_info_->last_text);
        
        // Calculate text dimensions
        auto text_dim = font_instance_->flow_line(utf32_text, INT_MAX);
        
        // Create surface with generous padding to ensure no character cutoff
        // Use font size as height reference and add extra width padding
        int padding = std::max(40, text_info_->last_font_size / 2);
        
        gpu::vec2i_t surface_size{
            static_cast<int>(text_dim.pixels_advanced) + padding * 2,  // More generous padding
            text_info_->last_font_size + padding * 2
        };

        if (!text_info_->surface || text_info_->surface->dimensions() != surface_size) {
            text_info_->surface = std::make_unique<render::surface_s>(surface_size);
        }

        // Clear surface to transparent
        text_info_->surface->clear({0, 0, 0, 0});

        // Position text with adequate padding from the top-left
        gpu::vec2i_t text_position{padding, text_info_->last_font_size + padding / 2};
        
        // Render text in white
        font_instance_->render_string(utf32_text, text_info_->surface.get(), text_position);

        // Transfer to GPU
        auto texture = text_info_->surface->texture();
        auto transfer = text_info_->surface->transfer();
        transfer->perform_copy();
        transfer->wait_for_copy();
        transfer->perform_transfer(texture);
        texture->generate_mip_maps();

        text_info_->needs_update = false;

        gpu::context_s::rewind_current();
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr || text_info_->last_text.empty()) {
            if (fb == nullptr) {
                spdlog::get("app")->debug("Text node: No framebuffer input");
            }
            if (text_info_->last_text.empty()) {
                spdlog::get("app")->debug("Text node: No text to render");
            }
            return;
        }

        spdlog::get("app")->debug("Text node executing with text: '{}'", text_info_->last_text);

        // Update text if needed
        if (text_info_->needs_update) {
            spdlog::get("app")->debug("Text node: Updating text rendering");
            render_text(app, state);
        }

        // Skip rendering if no surface is ready
        if (!text_info_->surface) {
            spdlog::get("app")->debug("Text node: No surface ready for rendering");
            return;
        }

        auto position = iface_position_in_.resolve_value(app, nodes, state, {0.0, 0.0});

        auto shader = draw_state_->get_shader_program();
        shader->set_uniform("opacity", 1.0);

        fb->bind();

        // Calculate the scale to render text at its natural pixel size
        // Convert surface dimensions to framebuffer coordinates  
        const gpu::vec2i_t fb_dim = fb->texture()->texture_dimensions();
        auto surface_size = text_info_->surface->dimensions();
        
        gpu::vec2_t scale{
            static_cast<float>(surface_size.x) / static_cast<float>(fb_dim.x),
            static_cast<float>(surface_size.y) / static_cast<float>(fb_dim.y)
        };
        
        shader->set_uniform("scale", scale);

        auto texture = text_info_->surface->texture();
        texture->bind(0);

        shader->set_uniform("offset", position);
        draw_state_->draw();

        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::unbind();
    }

    std::string_view type() const final { return "text"; }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Text"},
            {"text", "Hello World"},
            {"font_name", "Arial"},
            {"font_variant", "Regular"},
            {"font_size", 48}
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "text") {
            return value->is_string();
        }
        if (name == "font_name") {
            return value->is_string();
        }
        if (name == "font_variant") {
            return value->is_string();
        }
        if (name == "font_size") {
            return value->is_number_integer() && value->get<int>() > 0;
        }
        return node_i::is_valid_common_option(name, value);
    }
};

} // namespace

namespace miximus::nodes::text {

std::shared_ptr<node_i> create_text_node()
{
    return std::make_shared<node_impl>();
}

} // namespace miximus::nodes::text
