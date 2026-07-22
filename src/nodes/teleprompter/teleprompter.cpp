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

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;
using namespace boost::fibers;

class node_impl : public node_i
{
    struct line_info_s
    {
        std::mutex                                              mtx;
        ::future<void>                                          ready;
        int                                                     line_no{-1};
        uint64_t                                                upload_version{};
        std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream;
    };

    struct text_s
    {
        std::vector<std::u32string>              lines;
        std::unique_ptr<render::font_instance_s> font;
    };

    input_interface_s<gpu::rect_s>          iface_rect_in_{*this, "rect"};
    input_interface_s<double>               iface_scroll_pos_in_{*this, "scroll_pos"};
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_{*this, "fb_in"};
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_{*this, "fb_out"};

    std::unique_ptr<gpu::textured_quad_s>     textured_quad_;
    ::mutex                                   font_mtx_;
    std::shared_ptr<render::font_loader_s>    font_loader_ = std::make_shared<render::font_loader_s>();
    ::future<text_s>                          text_future_;
    text_s                                    text_;
    std::vector<std::unique_ptr<line_info_s>> render_lines_;

    utils::observed_value_s<gpu::vec2i_t> render_dimensions_;
    utils::observed_value_s<int>          font_size_;
    utils::observed_value_s<std::string>  file_path_;
    utils::observed_value_s<std::string>  font_name_;
    utils::observed_value_s<std::string>  font_variant_;
    utils::observed_value_s<std::string>  status_font_name_;
    utils::observed_value_s<uint64_t>     loaded_font_version_;
    utils::observed_value_s<uint64_t>     reported_font_version_;
    int                                   line_height_extra_{70};

  public:
    explicit node_impl() = default;

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    ~node_impl() override
    {
        for (auto& rl : render_lines_) {
            if (rl->ready.valid()) {
                try {
                    rl->ready.get();
                } catch (...) { // NOLINT(bugprone-empty-catch) -- destructor must not throw
                }
            }
        }
    }

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        const auto font_version      = app->font_registry()->get_font_list_version();
        const bool font_list_changed = reported_font_version_.observe(font_version);
        const auto font_name         = state.get_option<std::string_view>("font_name");
        const bool font_name_changed = status_font_name_.observe(font_name);

        if (font_list_changed) {
            app->status_registry()->write(id_, "font_names", app->font_registry()->get_font_options());
        }
        if (font_list_changed || font_name_changed) {
            app->status_registry()->write(
                id_, "font_variants", app->font_registry()->get_font_variant_options(font_name));
        }

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto       file_path    = state.get_option<std::string_view>("file_path");
        auto       font_name    = state.get_option<std::string_view>("font_name");
        auto       font_variant = state.get_option<std::string_view>("font_variant");
        const auto font_size    = state.get_option<int>("font_size", 100);

        auto fb = iface_fb_in_.resolve_value(app, nodes, state);
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto draw_rect = iface_rect_in_.resolve_value(app,
                                                      nodes,
                                                      state,
                                                      {
                                                          .pos  = {0,   0  },
                                                          .size = {1.0, 1.0},
        });

        auto scroll_pos = state.get_option<double>("scroll_pos", 0);
        scroll_pos      = iface_scroll_pos_in_.resolve_value(app, nodes, state, scroll_pos);

        const gpu::vec2i_t fb_dim   = fb->texture()->texture_dimensions();
        const gpu::recti_s viewport = gpu::normalized_to_pixel_rect(draw_rect, fb_dim);
        if (viewport.size.x <= 0 || viewport.size.y <= 0) {
            return;
        }

        const gpu::vec2i_t tx_dim = {viewport.size.x, font_size * 2};

        const auto font_version = app->font_registry()->get_font_list_version();
        const bool render_settings_changed =
            file_path_.would_change(file_path) || render_dimensions_.would_change(viewport.size) ||
            loaded_font_version_.would_change(font_version) || font_name_.would_change(font_name) ||
            font_variant_.would_change(font_variant) || font_size_.would_change(font_size);

        if (render_settings_changed) {
            if (text_future_.valid()) {
                if (text_future_.wait_for(0ms) != ::future_status::ready) {
                    return;
                }
                (void)text_future_.get();
            }

            // Line workers read the current font. Do not replace it until all
            // work using the previous text generation has finished.
            for (auto& line : render_lines_) {
                if (!line->ready.valid()) {
                    continue;
                }
                if (line->ready.wait_for(0ms) != ::future_status::ready) {
                    return;
                }
                line->ready.get();
            }

            file_path_.commit(file_path);
            render_dimensions_.commit(viewport.size);
            loaded_font_version_.commit(font_version);
            font_name_.commit(font_name);
            font_variant_.commit(font_variant);
            font_size_.commit(font_size);

            for (auto& rl : render_lines_) {
                rl->line_no = -1;
            }

            text_ = {};

            auto font_info = app->font_registry()->find_font_variant(font_name, font_variant);
            if (!font_info) {
                font_info = app->font_registry()->find_font_variant(render::get_default_font_name(), "Regular");
            }
            if (!font_info) {
                return;
            }

            auto future = app->thread_pool()->submit(
                load_file, font_loader_, *font_info, file_path_.value(), font_size_.value(), viewport.size.x);

            if (future) {
                text_future_ = std::move(*future);
            }

            assert(text_future_.valid());
        }

        if (text_future_.valid()) {
            if (text_future_.wait_for(0ms) == ::future_status::ready) {
                text_ = text_future_.get();
            } else {
                return;
            }
        }

        if (text_.lines.empty()) {
            return;
        }

        {
            // Resize render line vector, this can not be a simple resize, since
            // we need to wait for the future to finish
            const int total_line_height       = font_size_.value() + line_height_extra_;
            const int visible_lines_plus_four = ((viewport.size.y + total_line_height - 1) / total_line_height) + 4;

            while (render_lines_.size() < static_cast<size_t>(visible_lines_plus_four)) {
                render_lines_.emplace_back(std::make_unique<line_info_s>());
            }

            while (render_lines_.size() > static_cast<size_t>(visible_lines_plus_four)) {
                auto& rl = render_lines_.back();
                if (rl->ready.valid()) {
                    if (rl->ready.wait_for(0ms) != ::future_status::ready) {
                        break;
                    }
                    rl->ready.get();
                }

                render_lines_.pop_back();
            }
        }

        fb->begin_render(viewport);
        auto       batch = textured_quad_->begin_batch();
        const auto scale = gpu::pixels_to_normalized(gpu::vec2_t(tx_dim), viewport.size);

        /**
         * Iterate over render lines, starting 2 lines over visible area, ending 2 lines below
         * The size of render_lines_ is (visible lines + 4)
         */
        for (int i = -2; i < static_cast<int>(render_lines_.size()) - 2; ++i) {
            const int txt_line_index = static_cast<int>(std::floor(scroll_pos)) + i;
            if (txt_line_index < 0 || static_cast<size_t>(txt_line_index) >= text_.lines.size()) {
                continue;
            }

            auto& rl = render_lines_[txt_line_index % render_lines_.size()];

            if (rl->line_no != txt_line_index && !rl->ready.valid()) {
                if (!rl->upload_stream || rl->upload_stream->desc().requirements.dimensions != tx_dim) {
                    const auto byte_size = sizeof(render::surface_s::rgba_pixel_t) * static_cast<size_t>(tx_dim.x) *
                                           static_cast<size_t>(tx_dim.y);
                    const gpu::transfer::texture_transfer_requirements_s requirements{
                        .dimensions        = tx_dim,
                        .format            = gpu::texture_s::format_e::rgba_f16,
                        .row_stride        = sizeof(render::surface_s::rgba_pixel_t) * static_cast<size_t>(tx_dim.x),
                        .byte_size         = byte_size,
                        .address_alignment = render::surface_s::DATA_ALIGNMENT,
                        .host_access       = gpu::transfer::host_access_e::read_write,
                    };
                    rl->upload_stream = app->texture_upload_service()->create_stream({
                        .requirements = requirements,
                        .max_slots    = 2,
                    });
                }

                auto upload = rl->upload_stream->try_acquire();
                if (!upload) {
                    continue;
                }

                // The render line contains the wrong text line AND is available for processing
                rl->line_no        = txt_line_index;
                rl->upload_version = upload->version();

                auto t = text_.lines[txt_line_index];

                auto future = app->thread_pool()->submit(
                    &node_impl::process_line, this, rl.get(), std::move(t), tx_dim, std::move(*upload));
                assert(future);

                if (future) {
                    rl->ready = std::move(*future);
                }
                continue;
            }

            if (rl->ready.valid()) {
                // Render line has active processing

                if (rl->ready.wait_for(0ms) == ::future_status::ready) {
                    // Processing is done
                    rl->ready.get();
                    if (rl->line_no == txt_line_index) {
                        // The upload service publishes the new texture when ready.
                    } else {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            const std::unique_lock lock(rl->mtx);
            auto                   texture = rl->upload_stream ? rl->upload_stream->consume_latest() : nullptr;
            if (texture == nullptr || rl->upload_stream->current_version() != rl->upload_version) {
                continue;
            }

            const int    line_height_px = font_size_.value() + line_height_extra_;
            const double px_pos         = std::floor((txt_line_index - scroll_pos) * line_height_px);
            const auto   pos            = gpu::pixels_to_normalized({0, px_pos}, viewport.size);

            batch.draw(texture, {.pos = pos, .size = scale});
        }
        gpu::framebuffer_s::end_render();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",         "Teleprompter"                              },
            {"file_path",    ""                                          },
            {"scroll_pos",   0                                           },
            {"font_name",    std::string(render::get_default_font_name())},
            {"font_variant", "Regular"                                   },
            {"font_size",    100                                         },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "file_path") {
            return normalize_option_value<std::string>(value);
        }

        if (name == "scroll_pos") {
            return normalize_option_value<double>(value, 0);
        }

        if (name == "font_size") {
            return normalize_option_value<int>(value, 10, 100);
        }

        if (name == "font_name" || name == "font_variant") {
            return normalize_option_value<std::string>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "teleprompter"; }

    static text_s load_file(const std::shared_ptr<render::font_loader_s>& loader,
                            const render::font_variant_s&                 font_info,
                            const std::filesystem::path&                  path,
                            int                                           font_size,
                            int                                           width)
    {
        text_s         res = {};
        std::u32string str;

        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                return res;
            }

            const std::string utf_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            str = utils::utf8_to_utf32(utf_str);
        } catch (const std::ifstream::failure& e) {
            return res;
        }

        res.font = loader->load_font(&font_info);

        if (!res.font) {
            return res;
        }

        res.font->set_size(font_size);

        size_t pos = 0;

        while (pos < str.size()) {
            auto info = res.font->flow_line(str.substr(pos), width);
            res.lines.emplace_back(str.data() + pos, info.consumed_chars);
            pos += info.consumed_chars;
        }

        return res;
    }

    void process_line(line_info_s*                          line,
                      const std::u32string&                 str,
                      const gpu::vec2i_t&                   dim,
                      gpu::transfer::texture_upload_lease_s upload)
    {
        const std::unique_lock line_lock(line->mtx);
        const std::unique_lock font_lock(font_mtx_);

        render::surface_s surface(dim, upload.bytes());
        surface.clear({0, 0, 0, 0});
        text_.font->render_string(str, &surface, {0, font_size_.value()});
        upload.submit();
    }
};

} // namespace

namespace miximus::nodes::teleprompter {

std::shared_ptr<node_i> create_teleprompter_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::teleprompter
