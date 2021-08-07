#include "screen.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/glad.hpp"
#include "gpu/shader.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"

#include <memory>
#include <queue>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

class node_impl : public node_i
{
    struct fb_info_s
    {
        std::unique_ptr<gpu::framebuffer_s> framebuffer;
        std::unique_ptr<gpu::sync_s>        draw_sync;
        gpu::vec2i_t                        screen_size;
        // std::unique_ptr<gpu::sync_s>        render_sync;
    };

    input_interface_s<gpu::texture_s*> iface_tex_;
    std::unique_ptr<gpu::context_s>    context_;
    std::unique_ptr<gpu::draw_state_s> draw_state_;

    std::mutex              frame_mtx_;
    std::condition_variable frame_cv_;
    std::queue<fb_info_s>   frames_rendered_;
    std::queue<fb_info_s>   frames_free_;

    bool              thread_run_{};
    std::future<bool> thread_future_{};

  public:
    explicit node_impl() { interfaces_.emplace("tex", &iface_tex_); }

    ~node_impl() override { stop_thread(); }

    node_impl(const node_impl&) = delete;
    node_impl(node_impl&&)      = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&) = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        auto enabled = state.get_option<bool>("enabled", false);

        if (enabled) {
            traits->must_run = true;

            if (!context_) {
                std::unique_lock lock(frame_mtx_);

                context_    = gpu::context_s::create_unique_context(true, app->ctx());
                thread_run_ = true;

                for (int i = 0; i < 4; ++i) {
                    frames_free_.emplace();
                }

                thread_future_ = std::async(&node_impl::run, this);
            }

            gpu::recti_s rect{};
            auto         fullscreen   = state.get_option<bool>("fullscreen", false);
            auto         monitor_name = state.get_option<std::string>("monitor_name");
            rect.pos.x                = state.get_option<int>("posx", 0);
            rect.pos.y                = state.get_option<int>("posy", 0);
            rect.size.x               = state.get_option<int>("sizex", 100);
            rect.size.y               = state.get_option<int>("sizey", 100);

            if (fullscreen) {
                context_->set_fullscreen_monitor(monitor_name, rect);
            } else {
                context_->set_window_rect(rect);
            }
        } else if (context_) {
            stop_thread();
            context_.reset();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (!context_) {
            return;
        }

        auto*        texture = iface_tex_.resolve_value(app, nodes, state.get_connection_set("tex"));
        gpu::vec2i_t dim{128, 128};
        if (texture != nullptr) {
            dim = texture->texture_dimensions();
        }

        fb_info_s frame;

        {
            std::unique_lock lock(frame_mtx_);
            if (frames_free_.size() < 2) {
                // getlog("app")->warn("No frame available for render");
                return;
            }

            frame = std::move(frames_free_.front());
            frames_free_.pop();
        }

        // if (frame.render_sync) {
        //     frame.render_sync->gpu_wait();
        //     frame.render_sync.reset();
        // }

        if (!frame.framebuffer || frame.framebuffer->get_texture()->texture_dimensions() != dim) {
            frame.framebuffer = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::colorspace_e::RGB);
        }

        frame.framebuffer->bind();

        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 0, 0, 0);

        glClear(GLbitfield(GL_COLOR_BUFFER_BIT) | GLbitfield(GL_DEPTH_BUFFER_BIT));

        if (texture != nullptr) {
            if (!draw_state_) {
                draw_state_  = std::make_unique<gpu::draw_state_s>();
                auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
                draw_state_->set_shader_program(shader);
                draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
            }

            auto* shader = draw_state_->get_shader_program();
            shader->set_uniform("offset", gpu::vec2_t{0, 0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, 1.0});

            texture->bind(0);
            draw_state_->draw();
            gpu::texture_s::unbind(0);
        }

        gpu::framebuffer_s::unbind();

        frame.draw_sync   = std::make_unique<gpu::sync_s>();
        frame.screen_size = context_->get_framebuffer_size();

        {
            std::unique_lock lock(frame_mtx_);
            frames_rendered_.emplace(std::move(frame));
        }

        frame_cv_.notify_one();
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Screen output"},
            {"enabled", true},
            {"fullscreen", false},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "enabled" || name == "fullscreen") {
            return value.is_boolean();
        }

        if (name == "monitor_name") {
            return value.is_string();
        }

        if (name == "posx" || name == "posy") {
            return value.is_number_integer();
        }

        if (name == "sizex" || name == "sizey") {
            return value.is_number_integer() && value.get<int>() > 100;
        }

        return false;
    }

    std::string_view type() const final { return "screen_output"; }

    bool run()
    {
        auto log = getlog("app");

        context_->make_current();
        gpu::draw_state_s draw_state;
        auto*             shader = context_->get_shader(gpu::shader_program_s::name_e::basic);
        draw_state.set_shader_program(shader);
        draw_state.set_vertex_data(gpu::full_screen_quad_verts);
        shader->set_uniform("offset", gpu::vec2_t{0, 0});
        shader->set_uniform("scale", gpu::vec2_t{1.0, 1.0});

        while (true) {
            fb_info_s frame;

            {
                std::unique_lock lock(frame_mtx_);
                frame_cv_.wait(lock, [this]() { return !thread_run_ || !frames_rendered_.empty(); });

                if (!thread_run_) {
                    break;
                }

                if (frames_rendered_.size() > 2) {
                    auto& f = frames_rendered_.front();
                    frames_free_.emplace(std::move(f));
                    frames_rendered_.pop();
                    log->warn("Discarding frame");
                }

                if (!frames_rendered_.empty()) {
                    frame = std::move(frames_rendered_.front());
                    frames_rendered_.pop();
                }
            }

            if (frame.draw_sync && frame.framebuffer) {
                frame.draw_sync->gpu_wait();
                frame.draw_sync.reset();

                auto* texture = frame.framebuffer->get_texture();
                auto  dim     = frame.screen_size;

                glViewport(0, 0, dim.x, dim.y);
                glClearColor(0, 0, 0, 0);

                texture->generate_mip_maps();
                texture->bind(0);
                draw_state.draw();
                gpu::texture_s::unbind(0);

                context_->swap_buffers();

                // gpu::sync_s sync;
                // while (!sync.cpu_wait(0ms)) {
                //     std::this_thread::sleep_for(1ms);
                // }

                {
                    std::unique_lock lock(frame_mtx_);
                    frames_free_.push(std::move(frame));
                }

                // frame.render_sync = std::make_unique<gpu::sync_s>();
            }
        }

        gpu::context_s::rewind_current();

        return true;
    }

    void stop_thread()
    {
        {
            std::unique_lock lock(frame_mtx_);
            thread_run_ = false;

            while (!frames_rendered_.empty()) {
                frames_rendered_.pop();
            }

            while (!frames_free_.empty()) {
                frames_free_.pop();
            }
        }

        frame_cv_.notify_one();

        if (thread_future_.valid()) {
            thread_future_.get();
        }
    }
}; // namespace

} // namespace

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
