#include "teleprompter.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"

#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <queue>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

std::u32string decode_utf8(const std::string& utf8_string)
{
    struct destructible_codecvt : public std::codecvt<char32_t, char, std::mbstate_t>
    {
        using std::codecvt<char32_t, char, std::mbstate_t>::codecvt;
        ~destructible_codecvt() = default;
    };

    std::wstring_convert<destructible_codecvt, char32_t> utf32_converter;
    return utf32_converter.from_bytes(utf8_string);
}

class node_impl : public node_i
{
    struct line_info_s
    {
        std::mutex                         mtx;
        std::atomic_bool                   queued{false};
        std::atomic_bool                   ready{false};
        bool                               transfered{false};
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

    std::unique_ptr<gpu::draw_state_s> draw_state_;
    render::font_loader_s              font_loader_;

    gpu::vec2i_t last_framebuffer_size{0, 0};
    int          font_size_{80};
    int          line_height_extra_{50};
    std::string  file_path_;

    std::future<text_s>                       text_future_;
    text_s                                    text_;
    std::vector<std::unique_ptr<line_info_s>> render_lines_;

    std::mutex               process_mtx_;
    std::condition_variable  process_cv_;
    std::queue<line_info_s*> process_lines_;
    std::thread              process_thread_;
    bool                     process_thread_run_{true};

  public:
    explicit node_impl()
        : process_thread_([this] { run_thread(); })
    {
        interfaces_.emplace("scroll_pos", &iface_scroll_pos_in_);
        interfaces_.emplace("fb_in", &iface_fb_in_);
        interfaces_.emplace("fb_out", &iface_fb_out_);
    }

    ~node_impl()
    {
        if (text_future_.valid()) {
            text_future_.wait();
        }

        {
            std::unique_lock lock(process_mtx_);
            process_thread_run_ = false;
        }

        process_cv_.notify_one();
        process_thread_.join();
    }

    void prepare(core::app_state_s* app, const node_state_s& /*nodes*/, traits_s* /*traits*/) final
    {
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

        double scroll_pos = state.get_option<double>("scroll_pos", 0);
        scroll_pos = iface_scroll_pos_in_.resolve_value(app, nodes, state.get_connection_set("scroll_pos"), scroll_pos);

        if (fb == nullptr) {
            return;
        }

        auto dim = fb->get_texture()->texture_dimensions();

        if (file_path != file_path_ || dim != last_framebuffer_size) {
            file_path_            = file_path;
            last_framebuffer_size = dim;

            for (auto& rl : render_lines_) {
                rl->line_no = -1;
            }

            if (text_future_.valid()) {
                text_future_.wait();
            }

            text_        = {};
            text_future_ = std::async(load_file, &font_loader_, app->font_registry(), file_path_, font_size_, dim.x);
            assert(text_future_.valid());
        }

        if (text_future_.valid()) {
            if (text_future_.wait_for(0ms) == std::future_status::ready) {
                text_ = std::move(text_future_.get());
            } else {
                return;
            }
        }

        if (text_.str.empty()) {
            return;
        }

        gpu::vec2i_t tx_dim{dim.x, font_size_ * 2};
        int          count = (dim.y + font_size_ - 1) / font_size_;

        {
            std::unique_lock lock(process_mtx_);

            while (render_lines_.size() < count + 4) {
                render_lines_.emplace_back(std::make_unique<line_info_s>());
            }

            while (render_lines_.size() > count + 4 && !render_lines_.back()->queued) {
                render_lines_.pop_back();
            }

            for (auto& rl : render_lines_) {
                if (!rl->surface || rl->surface->dimensions() != tx_dim) {
                    rl->surface = std::make_unique<render::surface_s>(tx_dim);
                    rl->line_no = -1;
                }
            }
        }

        int first_line = std::floor(scroll_pos);

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("scale", gpu::vec2_t{1, (float)-tx_dim.y / dim.y});

        fb->bind();

        for (int i = -2; i < count + 2; ++i) {
            int l = first_line + i;
            if (l < 0 || l >= text_.lines.size()) {
                continue;
            }

            int r = l % render_lines_.size();

            auto& rl = render_lines_[r];
            if (rl->line_no != l && !rl->queued) {
                rl->queued     = true;
                rl->ready      = false;
                rl->transfered = false;
                rl->line_no    = l;
                {
                    std::unique_lock lock(process_mtx_);
                    process_lines_.push(rl.get());
                }

                process_cv_.notify_one();
            }

            if (!rl->ready) {
                continue;
            }

            auto* texture = rl->surface->texture();

            if (!rl->transfered) {
                std::unique_lock lock(rl->mtx);
                auto*            transfer = rl->surface->transfer();
                transfer->perform_copy();
                transfer->wait_for_copy();
                transfer->perform_transfer(texture);
                rl->transfered = true;
            }

            texture->bind(0);

            int    line_height_px = font_size_ + line_height_extra_;
            double line_height    = (double)line_height_px / dim.y;
            double pos            = line_height * ((double)l - scroll_pos);

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
            {"size", nlohmann::json::array({1280, 720})},
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

    static text_s load_file(render::font_loader_s*   loader,
                            render::font_registry_s* registry,
                            std::filesystem::path    path,
                            int                      font_size,
                            int                      width)
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

        auto font_info = registry->find_font_variant("Ubuntu", "Regular");
        if (!font_info) {
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

    void run_thread()
    {
        while (true) {
            line_info_s* line{};

            {
                std::unique_lock lock(process_mtx_);
                process_cv_.wait(lock, [&]() { return !process_thread_run_ || !process_lines_.empty(); });
                if (!process_thread_run_) {
                    break;
                }

                if (process_lines_.empty()) {
                    continue;
                }

                line = process_lines_.front();
                process_lines_.pop();
            }

            std::unique_lock lock(line->mtx);

            line->surface->clear({0, 0, 0, 0});

            auto&               tl = text_.lines[line->line_no];
            std::u32string_view view(text_.str.data() + tl.first, tl.second);
            text_.font->draw_line(view, line->surface.get(), {0, font_size_});

            line->queued = false;
            line->ready  = true;
        }
    }
};

} // namespace

namespace miximus::nodes::teleprompter {

std::shared_ptr<node_i> create_teleprompter_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::teleprompter
