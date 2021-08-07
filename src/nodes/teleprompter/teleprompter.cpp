#include "teleprompter.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;
using namespace boost::fibers;

std::u32string decode_utf8(const std::string& utf8_string)
{
    struct destructible_codecvt : public std::codecvt<char32_t, char, std::mbstate_t>
    {
        using std::codecvt<char32_t, char, std::mbstate_t>::codecvt;
        ~destructible_codecvt() override                  = default;
        destructible_codecvt(const destructible_codecvt&) = delete;
        destructible_codecvt(destructible_codecvt&&)      = delete;
        void operator=(const destructible_codecvt&) = delete;
        void operator=(destructible_codecvt&&) = delete;
    };

    std::wstring_convert<destructible_codecvt, char32_t> utf32_converter;
    return utf32_converter.from_bytes(utf8_string);
}

class node_impl : public node_i
{
    struct line_info_s
    {
        ::future<gpu::sync_s>              ready;
        int                                line_no{-1};
        std::unique_ptr<render::surface_s> surface;
    };

    struct text_s
    {
        std::u32string                           str;
        std::vector<std::pair<size_t, size_t>>   lines;
        std::unique_ptr<render::font_instance_s> font;
    };

    input_interface_s<double>               iface_scroll_pos_in_;
    input_interface_s<gpu::framebuffer_s*>  iface_fb_in_;
    output_interface_s<gpu::framebuffer_s*> iface_fb_out_;

    std::unique_ptr<gpu::draw_state_s>        draw_state_;
    ::mutex                                   font_mtx_;
    render::font_loader_s                     font_loader_;
    ::future<text_s>                          text_future_;
    text_s                                    text_;
    std::vector<std::unique_ptr<line_info_s>> render_lines_;
    ::mutex                                   ctx_mtx_;
    std::unique_ptr<gpu::context_s>           ctx_;

    gpu::vec2i_t last_framebuffer_size{0, 0};
    int          font_size_{80};
    int          line_height_extra_{50};
    std::string  last_file_path_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("scroll_pos", &iface_scroll_pos_in_);
        interfaces_.emplace("fb_in", &iface_fb_in_);
        interfaces_.emplace("fb_out", &iface_fb_out_);
    }

    node_impl(const node_impl&) = delete;
    node_impl(node_impl&&)      = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&) = delete;

    ~node_impl() override
    {
        if (text_future_.valid()) {
            text_future_.wait();
        }

        for (auto& rl : render_lines_) {
            if (rl->ready.valid()) {
                rl->ready.get();
            }
        }
    }

    void prepare(core::app_state_s* app, const node_state_s& /*nodes*/, traits_s* /*traits*/) final
    {
        if (!ctx_) {
            ctx_ = gpu::context_s::create_unique_context(false, app->ctx());
        }

        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto file_path = state.get_option<std::string_view>("file_path");

        auto* fb = iface_fb_in_.resolve_value(app, nodes, state.get_connection_set("fb_in"));
        iface_fb_out_.set_value(fb);

        if (fb == nullptr) {
            return;
        }

        auto scroll_pos = state.get_option<double>("scroll_pos", 0);
        scroll_pos = iface_scroll_pos_in_.resolve_value(app, nodes, state.get_connection_set("scroll_pos"), scroll_pos);

        gpu::vec2i_t fb_dim = fb->texture()->texture_dimensions();
        gpu::vec2i_t tx_dim = {fb_dim.x, font_size_ * 2};

        if (file_path != last_file_path_ || fb_dim != last_framebuffer_size) {
            last_file_path_       = file_path;
            last_framebuffer_size = fb_dim;

            for (auto& rl : render_lines_) {
                rl->line_no = -1;
            }

            if (text_future_.valid()) {
                text_future_.get();
            }

            text_ = {};

            const auto* font_info = app->font_registry()->find_font_variant("Liberation Sans", "Regular");
            if (font_info == nullptr) {
                font_info = app->font_registry()->find_font_variant("Arial", "Regular");
            }
            if (font_info == nullptr) {
                return;
            }

            auto future =
                app->thread_pool()->submit(load_file, &font_loader_, font_info, last_file_path_, font_size_, fb_dim.x);

            if (future) {
                text_future_ = std::move(*future);
            }

            assert(text_future_.valid());
        }

        if (text_future_.valid()) {
            if (text_future_.wait_for(0ms) == ::future_status::ready) {
                text_ = std::move(text_future_.get());
            } else {
                return;
            }
        }

        if (text_.str.empty()) {
            return;
        }

        {
            // Resize render line vector, this can not be a simple resize, since
            // we need to wait for the future to finish
            int total_line_height       = font_size_ + line_height_extra_;
            int visible_lines_plus_four = (fb_dim.y + total_line_height - 1) / total_line_height + 4;

            while (render_lines_.size() < visible_lines_plus_four) {
                render_lines_.emplace_back(std::make_unique<line_info_s>());
            }

            while (render_lines_.size() > visible_lines_plus_four) {
                auto& rl = render_lines_.back();
                if (rl->ready.valid()) {
                    rl->ready.get();
                }

                render_lines_.pop_back();
            }
        }

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("scale", gpu::vec2_t{1, static_cast<float>(-tx_dim.y) / fb_dim.y});

        fb->bind();

        /**
         * Iterate over render lines, starting 2 lines over visible area, ending 2 lines below
         * The size of render_lines_ is (visible lines + 4)
         */
        for (int i = -2; i < static_cast<int>(render_lines_.size()) - 2; ++i) {
            int txt_line_index = static_cast<int>(std::floor(scroll_pos)) + i;
            if (txt_line_index < 0 || txt_line_index >= text_.lines.size()) {
                continue;
            }

            auto& rl = render_lines_[txt_line_index % render_lines_.size()];

            if (rl->line_no != txt_line_index && !rl->ready.valid()) {
                // The render line contains the wrong text line AND is available for processing
                rl->line_no = txt_line_index;

                if (!rl->surface || rl->surface->dimensions() != tx_dim) {
                    rl->surface = std::make_unique<render::surface_s>(tx_dim);
                }

                auto&          t = text_.lines[txt_line_index];
                std::u32string view(text_.str.data() + t.first, t.second);

                auto future = app->thread_pool()->submit(&node_impl::process_line, this, rl.get(), std::move(view));

                if (future) {
                    rl->ready = std::move(*future);
                }
                continue;
            }

            auto* texture = rl->surface->texture();

            if (rl->ready.valid()) {
                // Render line has active processing

                if (rl->ready.wait_for(0ms) == ::future_status::ready) {
                    // Processing is done
                    auto sync = rl->ready.get();
                    if (rl->line_no == txt_line_index) {
                        /**
                         * Render line contains corrent line number after processing.
                         * During high speed scrolling this might get out of sync, so it
                         * can't be assumed that the line no is correct.
                         */
                        sync.gpu_wait();
                    } else {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            texture->bind(0);

            int    line_height_px = font_size_ + line_height_extra_;
            double px_height      = 1.0 / fb_dim.y;

            int px_pos = line_height_px * txt_line_index - (scroll_pos * line_height_px);

            double pos = px_height * px_pos;

            shader->set_uniform("offset", gpu::vec2_t{0, 1.0 - pos});
            draw_state_->draw();
        }

        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::unbind();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Teleprompter"},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "file_path") {
            return value.is_string();
        }

        if (name == "scroll_pos") {
            return value.is_number() && value >= 0;
        }

        if (name == "font_size") {
            return value.is_number_integer() && value > 10 && value < 100;
        }

        return false;
    }

    std::string_view type() const final { return "teleprompter"; }

    static text_s load_file(render::font_loader_s*        loader,
                            const render::font_variant_s* font_info,
                            std::filesystem::path&&       path,
                            int                           font_size,
                            int                           width)
    {
        text_s res = {};

        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                return res;
            }

            std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.str = decode_utf8(str);
        } catch (const std::ifstream::failure& e) {
            return res;
        }

        res.font = loader->load_font(font_info);

        if (!res.font) {
            return res;
        }

        res.font->set_size(font_size);

        size_t pos = 0;

        while (pos < res.str.size()) {
            auto info = res.font->flow_line(res.str.substr(pos), width);
            res.lines.emplace_back(pos, info.consumed_chars);
            pos += info.consumed_chars;
        }

        return res;
    }

    gpu::sync_s process_line(line_info_s* line, std::u32string&& str)
    {
        std::unique_lock lock(font_mtx_);

        line->surface->clear({0, 0, 0, 0});

        text_.font->draw_line(str, line->surface.get(), {0, font_size_});

        std::optional<gpu::sync_s> sync;

        {
            std::unique_lock lock(ctx_mtx_);
            ctx_->make_current();

            auto* texture  = line->surface->texture();
            auto* transfer = line->surface->transfer();
            transfer->perform_copy();
            transfer->wait_for_copy();
            transfer->perform_transfer(texture);
            texture->generate_mip_maps();
            sync.emplace();

            gpu::context_s::rewind_current();
        }

        return std::move(*sync);
    }
};

} // namespace

namespace miximus::nodes::teleprompter {

std::shared_ptr<node_i> create_teleprompter_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::teleprompter
