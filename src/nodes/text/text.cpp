#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/geometry.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "render/font/font_instance.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"
#include "utils/observed_value.hpp"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;
using namespace boost::fibers;

class node_impl : public node_i
{
    struct text_render_info_s
    {
        std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream;
        gpu::vec2i_t                                            surface_size{};
        bool                                                    needs_update{true};
        utils::observed_value_s<std::string>                    text;
        utils::observed_value_s<std::string>                    font_name;
        utils::observed_value_s<std::string>                    font_variant;
        utils::observed_value_s<int>                            font_size;
    };

    input_interface_s<gpu::vec2_t>          iface_position_in_{*this, "position"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<gpu::textured_quad_s>    textured_quad_;
    std::shared_ptr<render::font_loader_s>   font_loader_;
    std::unique_ptr<text_render_info_s>      text_info_;
    ::mutex                                  font_mtx_;
    std::unique_ptr<render::font_instance_s> font_instance_;
    utils::observed_value_s<uint64_t>        font_version_;
    utils::observed_value_s<std::string>     status_font_name_;

  public:
    explicit node_impl()
    {
        text_info_   = std::make_unique<text_render_info_s>();
        font_loader_ = std::make_shared<render::font_loader_s>();
    }

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        const auto font_version      = app->font_registry()->get_font_list_version();
        const bool font_list_changed = font_version_.observe(font_version);
        const auto font_name         = state.get_option<std::string_view>("font_name");
        const bool font_name_changed = status_font_name_.observe(font_name);

        if (font_list_changed) {
            text_info_->needs_update = true;
            app->status_registry()->write(id_, "font_names", app->font_registry()->get_font_names());
        }
        if (font_list_changed || font_name_changed) {
            app->status_registry()->write(
                id_, "font_variants", app->font_registry()->get_font_variant_names(font_name));
        }

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        // Check if text or font settings have changed
        auto text         = state.get_option<std::string_view>("text");
        auto font_variant = state.get_option<std::string_view>("font_variant");
        auto font_size    = state.get_option<int>("font_size");

        bool render_settings_changed = text_info_->text.observe(text);
        render_settings_changed |= text_info_->font_name.observe(font_name);
        render_settings_changed |= text_info_->font_variant.observe(font_variant);
        render_settings_changed |= text_info_->font_size.observe(font_size);

        if (render_settings_changed) {
            text_info_->needs_update = true;

            if (text.empty()) {
                text_info_->upload_stream.reset();
                text_info_->surface_size = {};
            }
        }

        if (text_info_->needs_update && !text_info_->text.value().empty()) {
            render_text(app, state);
        }
    }

    void render_text(core::app_state_s* app, [[maybe_unused]] const node_state_s& state)
    {
        // Load font if needed
        {
            const std::unique_lock<::mutex> font_lock(font_mtx_);

            std::optional<render::font_variant_s> font_info;

            spdlog::get("app")->info("Text rendering: '{}' with font '{}' size {}",
                                     text_info_->text.value(),
                                     text_info_->font_name.value(),
                                     text_info_->font_size.value());

            if (!text_info_->font_name.value().empty()) {
                font_info = app->font_registry()->find_font_variant(text_info_->font_name.value(),
                                                                    text_info_->font_variant.value());
            }

            if (!font_info) {
                font_info = app->font_registry()->find_font_variant(render::get_default_font_name(), "Regular");
            }

            if (!font_info) {
                // Try any available font
                auto font_names = app->font_registry()->get_font_names();
                if (!font_names.empty()) {
                    font_info = app->font_registry()->find_font_variant(font_names[0], "Regular");
                }
            }

            if (!font_info) {
                return;
            }

            // Load font instance
            font_instance_ = font_loader_->load_font(&*font_info);
            if (!font_instance_) {
                return;
            }

            // Set the font size
            font_instance_->set_size(text_info_->font_size.value());
        }

        // Convert text to UTF-32
        auto utf32_text = utils::utf8_to_utf32(text_info_->text.value());

        // Calculate text dimensions
        auto text_dim = font_instance_->flow_line(utf32_text, INT_MAX);

        // Create surface with generous padding to ensure no character cutoff
        // Use font size as height reference and add extra width padding
        const int padding = std::max(40, text_info_->font_size.value() / 2);

        const gpu::vec2i_t surface_size{static_cast<int>(text_dim.pixels_advanced) +
                                            (padding * 2), // More generous padding
                                        text_info_->font_size.value() + (padding * 2)};

        if (!text_info_->upload_stream || text_info_->surface_size != surface_size) {
            text_info_->surface_size = surface_size;
            const auto byte_size     = sizeof(render::surface_s::rgba_pixel_t) * static_cast<size_t>(surface_size.x) *
                                   static_cast<size_t>(surface_size.y);
            text_info_->upload_stream = app->texture_upload_service()->create_stream({
                .dimensions = surface_size,
                .format     = gpu::texture_s::format_e::rgba_f16,
                .byte_size  = byte_size,
                .max_slots  = 3,
            });
        }

        auto upload = text_info_->upload_stream->try_acquire();
        if (!upload) {
            return;
        }

        render::surface_s surface(surface_size, static_cast<render::surface_s::rgba_pixel_t*>(upload->ptr()));
        surface.clear({0, 0, 0, 0});

        // Position text with adequate padding from the top-left
        const gpu::vec2i_t text_position{padding, text_info_->font_size.value() + (padding / 2)};

        // Render text in white
        font_instance_->render_string(utf32_text, &surface, text_position);
        upload->submit();

        text_info_->needs_update = false;
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr || !text_info_->text.has_value() || text_info_->text.value().empty()) {
            if (fb == nullptr) {
                spdlog::get("app")->debug("Text node: No framebuffer input");
            }
            if (!text_info_->text.has_value() || text_info_->text.value().empty()) {
                spdlog::get("app")->debug("Text node: No text to render");
            }
            return;
        }

        spdlog::get("app")->debug("Text node executing with text: '{}'", text_info_->text.value());

        // Update text if needed
        if (text_info_->needs_update) {
            spdlog::get("app")->debug("Text node: Updating text rendering");
            render_text(app, state);
        }

        auto texture = text_info_->upload_stream ? text_info_->upload_stream->consume_latest() : nullptr;
        if (texture == nullptr) {
            spdlog::get("app")->debug("Text node: No uploaded texture ready for rendering");
            return;
        }

        auto position = iface_position_in_.resolve_value(app, nodes, state, {0.0, 0.0});

        fb->begin_render();

        // Calculate the scale to render text at its natural pixel size
        // Convert surface dimensions to framebuffer coordinates
        const gpu::vec2i_t fb_dim       = fb->texture()->texture_dimensions();
        const auto         surface_size = text_info_->surface_size;

        const auto scale = gpu::pixels_to_normalized(gpu::vec2_t(surface_size), fb_dim);

        textured_quad_->draw(texture, {.pos = position, .size = scale});
        gpu::framebuffer_s::end_render();
    }

    std::string_view type() const final { return "text"; }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",         "Text"                                      },
            {"text",         "Hello World"                               },
            {"font_name",    std::string(render::get_default_font_name())},
            {"font_variant", "Regular"                                   },
            {"font_size",    48                                          },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "text" || name == "font_name" || name == "font_variant") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "font_size") {
            return normalize_option_value<int>(value, 1);
        }
        return option_result_e::invalid;
    }
};

} // namespace

namespace miximus::nodes::text {

std::shared_ptr<node_i> create_text_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::text
