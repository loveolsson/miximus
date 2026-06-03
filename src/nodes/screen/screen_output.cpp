#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"
#include "utils/frame_queue.hpp"
#include "utils/thread_priority.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

class node_impl : public node_i
{
    struct frame_info_s
    {
        GLuint                       id{};
        std::unique_ptr<gpu::sync_s> sync;
        gpu::vec2i_t                 tx_size{};
        gpu::vec2i_t                 fb_size{};

        frame_info_s() = default;

        frame_info_s(const frame_info_s&) = delete;
        frame_info_s(frame_info_s&& o) noexcept { *this = std::move(o); }
        frame_info_s& operator=(const frame_info_s&) = delete;
        frame_info_s& operator=(frame_info_s&& o) noexcept
        {
            if (id != 0) {
                glDeleteBuffers(1, &id);
            }

            id      = o.id;
            sync    = std::move(o.sync);
            tx_size = o.tx_size;
            fb_size = o.fb_size;

            o.id = 0;

            return *this;
        }

        ~frame_info_s()
        {
            if (id != 0) {
                glDeleteBuffers(1, &id);
            }
        }
    };

    // All state the render thread touches lives in render_state_s and is held
    // behind a shared_ptr so the thread lambda and node_impl share ownership.
    struct render_state_s
    {
        std::condition_variable            frame_cv;
        utils::frame_queue_s<frame_info_s> frames_rendered;
        utils::frame_queue_s<frame_info_s> frames_free;
        std::atomic<bool>                  thread_run{false};
        std::unique_ptr<gpu::context_s>    ctx;
    };

    input_interface_s<gpu::texture_s*> iface_tex_{"tex"};

    std::shared_ptr<render_state_s>     render_state_;
    std::thread                         render_thread_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;

  public:
    explicit node_impl() { register_interface(&iface_tex_); }

    ~node_impl() override { stop_thread(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        auto enabled = state.get_option<bool>("enabled", false);

        if (enabled) {
            traits->must_run = true;

            if (!render_state_) {
                render_state_             = std::make_shared<render_state_s>();
                render_state_->ctx        = gpu::context_s::create_unique_context(true, app->ctx());
                render_state_->thread_run = true;

                auto state_cap = render_state_;
                render_thread_ = std::thread([state_cap]() mutable { run_render(state_cap); });
            }

            gpu::recti_s rect{};
            auto         fullscreen   = state.get_option<bool>("fullscreen", false);
            auto         monitor_name = state.get_option<std::string>("monitor_name");
            rect.pos.x                = state.get_option<int>("posx", 0);
            rect.pos.y                = state.get_option<int>("posy", 0);
            rect.size.x               = state.get_option<int>("sizex", 100);
            rect.size.y               = state.get_option<int>("sizey", 100);

            if (fullscreen) {
                render_state_->ctx->set_fullscreen_monitor(monitor_name, rect);
            } else {
                render_state_->ctx->set_window_rect(rect);
            }
        } else if (render_state_) {
            stop_thread();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (!render_state_) {
            return;
        }

        auto texture = iface_tex_.resolve_value(app, nodes, state);

        gpu::vec2i_t dim{128, 128};
        if (texture != nullptr) {
            dim = texture->texture_dimensions();
        }

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::bgra_u8);
        }

        frame_info_s frame;

        // Avoid blocking the main thread with a wait by not using the 2 most recently
        // displayed buffers, as it might still be used by the display context
        decltype(render_state_->frames_free)::record_s record;
        if (render_state_->frames_free.pop_frame_if_count(3, &record)) {
            frame = std::move(record.frame);
        }

        if (frame.sync) {
            // Ensure buffer is done in the display context
            frame.sync->gpu_wait();
            frame.sync.reset();
        }

        if (frame.tx_size != dim || frame.id == 0) {
            frame_info_s new_frame;
            new_frame.tx_size = dim;

            glCreateBuffers(1, &new_frame.id);
            glNamedBufferData(new_frame.id, static_cast<GLsizeiptr>(dim.x) * dim.y * 4, nullptr, GL_DYNAMIC_COPY);
            frame = std::move(new_frame);
        }

        frame.fb_size = render_state_->ctx->get_framebuffer_size();

        framebuffer_->bind();

        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 0, 0, 0);

        glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) | static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));

        if (texture != nullptr) {
            if (!draw_state_) {
                draw_state_ = std::make_unique<gpu::draw_state_s>();
                auto shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
                draw_state_->set_shader_program(shader);
                draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
            }

            auto shader = draw_state_->get_shader_program();
            shader->set_uniform("offset", gpu::vec2_t{0, 0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, 1.0});
            shader->set_uniform("opacity", 1.0);

            texture->bind(0);
            draw_state_->draw();
            gpu::texture_s::unbind(0);
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, frame.id);
        glReadPixels(0, 0, dim.x, dim.y, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gpu::framebuffer_s::unbind();

        // EXPERIMENT: behaves better on Linux
        // glMemoryBarrierByRegion(GL_TEXTURE_FETCH_BARRIER_BIT);

        {
            if (render_state_->frames_rendered.size() < 3) {
                frame.sync = std::make_unique<gpu::sync_s>();
                render_state_->frames_rendered.push_frame(std::move(frame), app->frame_info.timestamp);
                render_state_->frame_cv.notify_one();
            } else {
                // TODO(Love): log or signal this
                render_state_->frames_free.push_frame(std::move(frame));
            }
        }
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Screen output"},
            {"enabled",    true           },
            {"fullscreen", false          },
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "enabled" || name == "fullscreen") {
            return validate_option<bool>(value);
        }

        if (name == "monitor_name") {
            return validate_option<std::string_view>(value);
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

    // render_state is passed by value (shared_ptr copy) so the thread keeps
    // the state alive for the duration of the render loop.
    static void run_render(std::shared_ptr<render_state_s> state)
    {
        state->ctx->make_current();
        glEnable(GL_FRAMEBUFFER_SRGB);
        // Don't block in swap_buffers waiting for the compositor's frame callback.
        // On Wayland the compositor throttles (or stops) delivering frame callbacks
        // for windows it considers hidden, which makes swap_buffers block indefinitely.
        // Rate limiting is handled by the main thread's 60 fps loop instead.
        glfwSwapInterval(0);

        {
            gpu::draw_state_s draw_state;
            auto              shader = state->ctx->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state.set_shader_program(shader);
            draw_state.set_vertex_data(gpu::full_screen_quad_verts);
            shader->set_uniform("offset", gpu::vec2_t{0, 1.0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, -1.0});
            shader->set_uniform("opacity", 1.0);

            std::unique_ptr<gpu::texture_s> texture;

            while (true) {
                frame_info_s frame;

                {
                    auto lock = state->frames_rendered.get_lock();
                    state->frame_cv.wait(lock, [&state]() {
                        return !state->thread_run || state->frames_rendered.size_while_lock_held() > 1;
                    });

                    if (!state->thread_run) {
                        break;
                    }
                }

                decltype(state->frames_rendered)::record_s record;
                if (state->frames_rendered.pop_frame_if_count(2, &record)) {
                    // Avoid waiting by buffering one frame, so that the frame being displayed
                    // is not currently being rendered
                    frame = std::move(record.frame);
                }

                if (frame.sync && frame.id != 0) {
                    frame.sync->gpu_wait();
                    frame.sync.reset();

                    if (!texture || texture->texture_dimensions() != frame.tx_size) {
                        texture = std::make_unique<gpu::texture_s>(frame.tx_size, gpu::texture_s::format_e::bgra_u8);
                    }

                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.id);
                    glTextureSubImage2D(
                        texture->id(), 0, 0, 0, frame.tx_size.x, frame.tx_size.y, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

                    glViewport(0, 0, frame.fb_size.x, frame.fb_size.y);
                    glClearColor(0, 0, 0, 0);
                    glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) |
                            static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));

                    // EXPERIMENT: behaves better on Linux
                    // glTextureBarrier();

                    texture->bind(0);
                    draw_state.draw();
                    gpu::texture_s::unbind(0);

                    if (!state->thread_run) {
                        break;
                    }

                    state->ctx->swap_buffers();

                    frame.sync = std::make_unique<gpu::sync_s>();

                    state->frames_free.push_frame(std::move(frame));
                }
            }

            // Drain queued frames (and their PBOs) while the context is still
            // current, then destroy draw_state and texture. ✓
            state->frames_rendered.clear();
            state->frames_free.clear();
        } // draw_state, texture destroyed here — context still current ✓

        gpu::context_s::rewind_current();
    }

    void stop_thread()
    {
        if (!render_thread_.joinable()) {
            return;
        }

        {
            auto lock                 = render_state_->frames_rendered.get_lock();
            render_state_->thread_run = false;
        }

        render_state_->frames_rendered.clear();
        render_state_->frames_free.clear();
        render_state_->frame_cv.notify_one();
        render_thread_.join();
        render_state_.reset();
    }
};

} // namespace

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
