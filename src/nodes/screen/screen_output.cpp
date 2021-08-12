#include "core/app_state.hpp"
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
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"

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
        GLuint                       id{};
        std::unique_ptr<gpu::sync_s> sync;
        gpu::vec2i_t                 tx_size;
        gpu::vec2i_t                 fb_size;
    };

    input_interface_s<gpu::texture_s*>  iface_tex_;
    std::unique_ptr<gpu::context_s>     context_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

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

        auto* texture = iface_tex_.resolve_value(app, nodes, state.get_connection_set("tex"));

        gpu::vec2i_t dim{128, 128};
        if (texture != nullptr) {
            dim = texture->texture_dimensions();
        } else {
            int i = 0;
        }

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::bgra_u8);
        }

        fb_info_s frame;

        {
            std::unique_lock lock(frame_mtx_);
            // Avoid blocking the main thread with a wait by not using the 2 most recently
            // displayed buffers, as it might still be used by the display context

            if (frames_free_.size() > 2) {
                frame = std::move(frames_free_.front());
                frames_free_.pop();
            }
        }

        if (frame.sync) {
            // Ensure buffer is done in the display context
            frame.sync->gpu_wait();
            frame.sync.reset();
        }

        if (frame.tx_size != dim || frame.id == 0) {
            frame.tx_size = dim;

            glDeleteBuffers(1, &frame.id);

            glCreateBuffers(1, &frame.id);
            glNamedBufferData(frame.id, dim.x * dim.y * 4, nullptr, GL_DYNAMIC_COPY);
        }

        frame.fb_size = context_->get_framebuffer_size();

        framebuffer_->bind();

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
            shader->set_uniform("opacity", 1.0);

            texture->bind(0);
            draw_state_->draw();
            gpu::texture_s::unbind(0);
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, frame.id);
        glReadPixels(0, 0, dim.x, dim.y, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gpu::framebuffer_s::unbind();

        // EXPERIMENT: behaves better on Linux
        // glMemoryBarrierByRegion(GL_TEXTURE_FETCH_BARRIER_BIT);

        {
            std::unique_lock lock(frame_mtx_);

            if (frames_rendered_.size() < 3) {
                frame.sync = std::make_unique<gpu::sync_s>();
                frames_rendered_.emplace(std::move(frame));
                lock.unlock();
                frame_cv_.notify_one();
            } else {
                // TODO: log or signal this
                frames_free_.emplace(std::move(frame));
            }
        }
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Screen output"},
            {"enabled", true},
            {"fullscreen", false},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "enabled" || name == "fullscreen") {
            return validate_option<bool>(value);
        }

        if (name == "monitor_name") {
            return validate_option<std::string>(value);
        }

        if (name == "posx" || name == "posy") {
            return validate_option<int>(value);
        }

        if (name == "sizex" || name == "sizey") {
            return validate_option<int>(value, 100);
        }

        return false;
    }

    std::string_view type() const final { return "screen_output"; }

    bool run()
    {
        auto log = getlog("nodes");

        context_->make_current();
        gpu::draw_state_s draw_state;
        auto*             shader = context_->get_shader(gpu::shader_program_s::name_e::basic);
        draw_state.set_shader_program(shader);
        draw_state.set_vertex_data(gpu::full_screen_quad_verts);
        shader->set_uniform("offset", gpu::vec2_t{0, 1.0});
        shader->set_uniform("scale", gpu::vec2_t{1.0, -1.0});
        shader->set_uniform("opacity", 1.0);

        std::unique_ptr<gpu::texture_s> texture;

        while (true) {
            fb_info_s frame;

            {
                std::unique_lock lock(frame_mtx_);
                frame_cv_.wait(lock, [this]() { return !thread_run_ || frames_rendered_.size() > 1; });

                if (!thread_run_) {
                    break;
                }

                if (frames_rendered_.size() > 1) {
                    // Avoid waiting by buffering one frame, so that the frame being displayed
                    // is not currently being rendering
                    frame = std::move(frames_rendered_.front());
                    frames_rendered_.pop();
                }
            }

            if (frame.sync && frame.id != 0) {
                frame.sync->gpu_wait();
                frame.sync.reset();

                if (!texture || texture->texture_dimensions() != frame.tx_size) {
                    texture = std::make_unique<gpu::texture_s>(frame.tx_size, gpu::texture_s::format_e::bgra_u8);
                }

                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.id);
                glTextureSubImage2D(
                    texture->id(), 0, 0, 0, frame.tx_size.x, frame.tx_size.y, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

                glViewport(0, 0, frame.fb_size.x, frame.fb_size.y);
                glClearColor(0, 0, 0, 0);
                glClear(GLbitfield(GL_COLOR_BUFFER_BIT) | GLbitfield(GL_DEPTH_BUFFER_BIT));

                // EXPERIMENT: behaves better on Linux
                // glTextureBarrier();

                texture->bind(0);
                draw_state.draw();
                gpu::texture_s::unbind(0);

                context_->swap_buffers();

                frame.sync = std::make_unique<gpu::sync_s>();

                {
                    std::unique_lock lock(frame_mtx_);
                    frames_free_.emplace(std::move(frame));
                }
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
                glDeleteBuffers(1, &frames_rendered_.front().id);
                frames_rendered_.pop();
            }

            while (!frames_free_.empty()) {
                glDeleteBuffers(1, &frames_free_.front().id);
                frames_free_.pop();
            }
        }

        frame_cv_.notify_one();

        if (thread_future_.valid()) {
            thread_future_.get();
        }
    }
};

} // namespace

namespace miximus::nodes::screen {
std::shared_ptr<node_i> create_screen_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
