#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/drawing.hpp"
#include "gpu/geometry.hpp"
#include "gpu/texture.hpp"
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
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <glm/common.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

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

    input_interface_s<gpu::vec2_t>                iface_position_in_{*this, "position"};
    input_interface_s<framebuffer_source_info_s>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<framebuffer_source_info_s> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<text_render_info_s>                    text_info_{std::make_unique<text_render_info_s>()};
    utils::cpu_task_s<std::unique_ptr<text_render_info_s>> render_future_;
    uint64_t                                               requested_generation_{};
    uint64_t                                               rendering_generation_{};
    utils::observed_value_s<uint64_t>                      font_version_;
    utils::observed_value_s<std::string>                   status_font_name_;
    gpu::texture_frame_ptr                                 rendered_text_frame_;

  public:
    node_impl()                            = default;
    node_impl(const node_impl&)            = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl& operator=(node_impl&&)      = delete;

    ~node_impl() override { (void)render_future_.cancel(); }

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        const auto font_version      = app->font_registry()->get_font_list_version();
        const bool font_list_changed = font_version_.observe(font_version);
        const auto font_name         = state.get_option<std::string_view>("font_name");
        const bool font_name_changed = status_font_name_.observe(font_name);

        if (font_list_changed) {
            text_info_->needs_update = true;
            app->status_registry()->write(
                status_handle_,
                status::font_names_status_s{.font_names = app->font_registry()->get_font_options()},
                core::status_delivery_e::immediate);
        }
        if (font_list_changed || font_name_changed) {
            app->status_registry()->write(
                status_handle_,
                status::font_variants_status_s{
                    .font_variants = app->font_registry()->get_font_variant_options(font_name),
                },
                core::status_delivery_e::immediate);
        }

        // Check if text or font settings have changed
        auto text         = state.get_option<std::string_view>("text");
        auto font_variant = state.get_option<std::string_view>("font_variant");
        auto font_size    = state.get_option<int>("font_size");

        bool render_settings_changed = text_info_->text.observe(text);
        render_settings_changed |= text_info_->font_name.observe(font_name);
        render_settings_changed |= text_info_->font_variant.observe(font_variant);
        render_settings_changed |= text_info_->font_size.observe(font_size);

        if (render_settings_changed || font_list_changed) {
            ++requested_generation_;
            if (render_future_.cancel()) {
                render_future_ = {};
            }
            text_info_->needs_update = true;
            // A new generation must not reuse an obsolete task's upload stream.
            text_info_->upload_stream.reset();

            if (text.empty()) {
                text_info_->surface_size = {};
            }
        }

        if (text_info_->needs_update && !text_info_->text.value().empty()) {
            schedule_render(app);
        }
    }

    void schedule_render(core::app_state_s* app)
    {
        if (render_future_.valid()) {
            if (render_future_.wait_for(0ms) != std::future_status::ready) {
                return;
            }
            try {
                auto result = render_future_.get();
                if (rendering_generation_ == requested_generation_) {
                    text_info_ = std::move(result);
                }
            } catch (const std::exception& error) {
                getlog("app")->error("Text rendering failed: {}", error.what());
                if (rendering_generation_ == requested_generation_) {
                    text_info_->needs_update = false;
                }
            }
        }
        if (!text_info_->needs_update || text_info_->text.value().empty()) {
            return;
        }

        auto font_info =
            app->font_registry()->find_font_variant(text_info_->font_name.value(), text_info_->font_variant.value());
        if (!font_info) {
            font_info = app->font_registry()->find_font_variant(render::get_default_font_name(), "Regular");
        }
        if (!font_info) {
            const auto names = app->font_registry()->get_font_names();
            if (!names.empty()) {
                font_info = app->font_registry()->find_font_variant(names.front(), "Regular");
            }
        }
        if (!font_info) {
            return;
        }

        auto snapshot = std::make_unique<text_render_info_s>(*text_info_);
        auto future   = app->cpu_task_worker()->submit(
            utils::cpu_task_priority_e::normal,
            [info = std::move(snapshot), font = *font_info, uploads = app->texture_upload_service()]() mutable {
                render_text(uploads, *info, font);
                return std::move(info);
            });
        if (future) {
            rendering_generation_ = requested_generation_;
            render_future_        = std::move(*future);
        }
    }

    static void render_text(gpu::transfer::texture_upload_service_s* uploads,
                            text_render_info_s&                      info,
                            const render::font_variant_s&            font_info)
    {
        auto loader        = std::make_shared<render::font_loader_s>();
        auto font_instance = loader->load_font(&font_info);
        if (!font_instance) {
            info.needs_update = false;
            return;
        }
        font_instance->set_size(info.font_size.value());
        // Convert text to UTF-32
        auto utf32_text = utils::utf8_to_utf32(info.text.value());

        struct line_s
        {
            std::u32string_view text;
            gpu::vec2i_t        baseline;
        };
        const std::u32string_view text_view(utf32_text);
        std::vector<line_s>       lines;
        gpu::vec2i_t              minimum{};
        gpu::vec2i_t              maximum{};
        gpu::vec2i_t              baseline{};
        for (size_t offset = 0; offset < utf32_text.size();) {
            const auto remaining = text_view.substr(offset);
            const auto line      = font_instance->measure_line(remaining);
            lines.push_back({remaining.substr(0, line.text_length), baseline});
            if (line.metrics.has_ink) {
                const auto start = baseline + line.metrics.ink_bounds.pos;
                minimum          = glm::min(minimum, start);
                maximum          = glm::max(maximum, start + line.metrics.ink_bounds.size);
            }
            maximum = glm::max(maximum, baseline + line.metrics.advance);
            offset += line.consumed_length;
            baseline.y += font_instance->line_height();
        }
        const int          padding      = std::max(40, info.font_size.value() / 2);
        const gpu::vec2i_t surface_size = maximum - minimum + gpu::vec2i_t{padding * 2};

        if (!info.upload_stream || info.surface_size != surface_size) {
            info.surface_size                 = surface_size;
            const auto host_buffer_size_bytes = sizeof(render::surface_s::pixel_t) *
                                                static_cast<size_t>(surface_size.x) *
                                                static_cast<size_t>(surface_size.y);
            const gpu::transfer::host_frame_layout_s host_layout{
                .image_dimensions        = surface_size,
                .pixel_format            = gpu::transfer::host_pixel_format_e::rgba_u8,
                .row_stride_bytes        = sizeof(render::surface_s::pixel_t) * static_cast<size_t>(surface_size.x),
                .buffer_size_bytes       = host_buffer_size_bytes,
                .address_alignment_bytes = render::surface_s::PREFERRED_DATA_ALIGNMENT,
                .memory_access           = gpu::transfer::host_memory_access_e::read_write,
            };
            info.upload_stream = uploads->create_stream({
                .host_layout = host_layout,
                .max_slots   = 1,
            });
        }

        auto upload = info.upload_stream->try_acquire_upload_buffer();
        if (!upload) {
            if (info.upload_stream->allocation_failed()) {
                getlog("app")->error("Unable to allocate text surface");
                info.needs_update = false;
            }
            return;
        }

        render::surface_s surface(surface_size, upload->writable_host_bytes());
        surface.clear({0, 0, 0, 0});

        const auto text_origin = gpu::vec2i_t{padding} - minimum;
        for (const auto& line : lines) {
            font_instance->render_line(line.text, &surface, text_origin + line.baseline);
        }
        if (upload->submit()) {
            info.needs_update = false;
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        rendered_text_frame_.reset();
        const auto fb_source = iface_fb_in_.resolve_value(app, nodes, state);
        auto*      fb        = fb_source.texture;
        iface_fb_out_.set_value(fb_source);

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

        auto frame = text_info_->upload_stream ? text_info_->upload_stream->select_latest_completed_upload() : nullptr;
        if (!frame) {
            spdlog::get("app")->debug("Text node: No uploaded texture ready for rendering");
            return;
        }
        rendered_text_frame_ = std::move(frame);

        auto position = iface_position_in_.resolve_value(app, nodes, state, {0.0, 0.0});

        // Calculate the scale to render text at its natural pixel size
        // Convert surface dimensions to framebuffer coordinates
        const gpu::vec2i_t fb_dim       = fb->dimensions();
        const auto         surface_size = text_info_->surface_size;

        const auto scale = gpu::pixels_to_normalized(gpu::vec2_t(surface_size), fb_dim);

        const gpu::texture_draw_s geometry{
            .destination = {.pos = position, .size = scale}
        };
        gpu::draw_texture(app->commands(), rendered_text_frame_->texture(), fb, {.geometry = geometry});
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (rendered_text_frame_) {
            rendered_text_frame_.reset();
        }
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
