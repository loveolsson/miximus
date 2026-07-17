#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "utils/frame_queue.hpp"
#include "utils/observed_value.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <glm/common.hpp>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    static constexpr size_t FRAME_SLOT_COUNT = 4;

    struct frame_slot_s
    {
        // The root context owns the framebuffer. Its texture and the sync
        // objects are visible to the shared display context.
        std::unique_ptr<gpu::framebuffer_s> target;
        std::unique_ptr<gpu::sync_s>        ready;
    };

    // All state the render thread touches lives in render_state_s and is held
    // behind a shared_ptr so the thread lambda and node_impl share ownership.
    struct render_state_s
    {
        std::condition_variable                    frame_cv;
        utils::frame_queue_s<size_t>               frames_rendered;
        utils::frame_queue_s<size_t>               frames_free;
        std::array<frame_slot_s, FRAME_SLOT_COUNT> frame_slots;
        std::atomic<bool>                          thread_run{false};
        std::unique_ptr<gpu::context_s>            ctx;

        render_state_s()
        {
            for (size_t index = 0; index < frame_slots.size(); ++index) {
                frames_free.push_frame(index);
            }
        }
    };

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    std::shared_ptr<render_state_s>                   render_state_;
    std::thread                                       render_thread_;
    std::unique_ptr<gpu::draw_state_s>                draw_state_;
    utils::observed_value_s<std::vector<std::string>> monitor_names_;
    utils::observed_value_s<gpu::recti_s>             window_rect_;
    utils::observed_value_s<bool>                     fullscreen_;
    utils::observed_value_s<std::string>              monitor_name_;

  public:
    explicit node_impl() = default;

    ~node_impl() override { stop_thread(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        // Publish available monitors so the UI dropdown can populate.
        {
            std::vector<std::string> names;
            names.reserve(gpu::context_s::monitors_g.size());
            for (const auto& [name, _] : gpu::context_s::monitors_g) {
                names.push_back(name);
            }
            auto* sr = app->status_registry();
            if (monitor_names_.observe(names)) {
                sr->write(id_, "monitors", nlohmann::json(monitor_names_.value()));
            }
            sr->write(id_, "connected", render_state_ != nullptr);
        }

        auto enabled = state.get_option<bool>("enabled", false);

        if (enabled) {
            traits->must_run = true;

            const bool context_created = !render_state_;
            if (context_created) {
                render_state_             = std::make_shared<render_state_s>();
                render_state_->ctx        = gpu::context_s::create_unique_context(true, app->ctx());
                render_state_->thread_run = true;

                auto state_cap = render_state_;
                render_thread_ = std::thread([state_cap]() mutable { run_render(state_cap); });
            }

            const auto position = state.get_option<gpu::vec2_t>("position", {0, 0});
            const auto size     = state.get_option<gpu::vec2_t>("size", {100, 100});

            gpu::recti_s rect{
                .pos  = gpu::vec2i_t(glm::round(position)),
                .size = gpu::vec2i_t(glm::round(size)),
            };
            auto fullscreen   = state.get_option<bool>("fullscreen", false);
            auto monitor_name = state.get_option<std::string>("monitor_name");

            bool window_settings_changed = window_rect_.observe(rect);
            window_settings_changed |= fullscreen_.observe(fullscreen);
            window_settings_changed |= monitor_name_.observe(monitor_name);

            if (context_created || window_settings_changed) {
                if (fullscreen) {
                    render_state_->ctx->set_fullscreen_monitor(monitor_name, rect);
                } else {
                    render_state_->ctx->set_window_rect(rect);
                }
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

        decltype(render_state_->frames_free)::record_s record;
        if (!render_state_->frames_free.pop_frame(&record)) {
            return;
        }

        auto  slot_index = record.frame;
        auto& slot       = render_state_->frame_slots.at(slot_index);

        if (!slot.target || slot.target->texture()->texture_dimensions() != dim) {
            slot.target = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::bgra_u8);
        }

        slot.target->begin_render(gpu::framebuffer_s::load_op_e::clear);

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

        gpu::framebuffer_s::end_render();

        slot.ready = std::make_unique<gpu::sync_s>();
        // A fence consumed from another context must be flushed by its producer.
        gpu::context_s::flush();

        render_state_->frames_rendered.push_frame(slot_index, app->frame_info.timestamp);
        render_state_->frame_cv.notify_one();
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Screen output"      },
            {"enabled",    true                 },
            {"fullscreen", false                },
            {"position",   gpu::vec2_t{0, 0}    },
            {"size",       gpu::vec2_t{100, 100}},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "enabled" || name == "fullscreen") {
            return normalize_option_value<bool>(value);
        }

        if (name == "monitor_name") {
            return normalize_option_value<std::string_view>(value);
        }

        if (name == "position") {
            auto result = normalize_option_value<gpu::vec2_t>(value);
            if (result == option_result_e::invalid) {
                return result;
            }

            const auto normalized = value->get<gpu::vec2_t>();
            const auto rounded    = glm::round(normalized);
            *value                = rounded;
            return rounded == normalized ? result : option_result_e::corrected;
        }

        if (name == "size") {
            auto result = normalize_option_value<gpu::vec2_t>(value, gpu::vec2_t{100, 100});
            if (result == option_result_e::invalid) {
                return result;
            }

            const auto normalized = value->get<gpu::vec2_t>();
            const auto rounded    = glm::round(normalized);
            *value                = rounded;
            return rounded == normalized ? result : option_result_e::corrected;
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "screen_output"; }

    // render_state is passed by value (shared_ptr copy) so the thread keeps
    // the state alive for the duration of the render loop.
    static void run_render(std::shared_ptr<render_state_s> state)
    {
        const gpu::context_scope_s context_scope(*state->ctx);
        glEnable(GL_FRAMEBUFFER_SRGB);
        // Don't block in swap_buffers waiting for the compositor's frame callback.
        // On Wayland the compositor throttles (or stops) delivering frame callbacks
        // for windows it considers hidden, which makes swap_buffers block indefinitely.
        // Rate limiting is handled by the configured main render cadence instead.
        glfwSwapInterval(0);

        {
            gpu::draw_state_s draw_state;
            auto              shader = state->ctx->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state.set_shader_program(shader);
            draw_state.set_vertex_data(gpu::full_screen_quad_verts);
            shader->set_uniform("offset", gpu::vec2_t{0, 1.0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, -1.0});
            shader->set_uniform("opacity", 1.0);

            while (true) {
                {
                    auto lock = state->frames_rendered.get_lock();
                    state->frame_cv.wait(lock, [&state]() {
                        return !state->thread_run || state->frames_rendered.size_while_lock_held() > 0;
                    });

                    if (!state->thread_run) {
                        break;
                    }
                }

                decltype(state->frames_rendered)::record_s record;
                if (!state->frames_rendered.pop_frame(&record)) {
                    continue;
                }

                auto  slot_index = record.frame;
                auto& slot       = state->frame_slots.at(slot_index);

                if (slot.ready) {
                    slot.ready->gpu_wait();
                    slot.ready.reset();
                }

                const auto framebuffer_size = state->ctx->get_framebuffer_size();
                glViewport(0, 0, framebuffer_size.x, framebuffer_size.y);
                glClearColor(0, 0, 0, 0);
                glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) | static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));

                slot.target->texture()->bind(0);
                draw_state.draw();
                gpu::texture_s::unbind(0);

                state->ctx->swap_buffers();

                auto released = std::make_unique<gpu::sync_s>();
                gpu::context_s::flush();
                // Keep presentation completion off the render thread. A slot is
                // not advertised as free until the display context is finished
                // sampling it, so its next render never inherits a GPU wait.
                (void)released->cpu_wait(std::chrono::hours(1));
                released.reset();
                state->frames_free.push_frame(slot_index);
            }

        } // draw_state destroyed here while the display context is still current.
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

        render_state_->frame_cv.notify_one();
        render_thread_.join();

        render_state_->frames_rendered.clear();
        render_state_->frames_free.clear();
        render_state_.reset();
    }
};

} // namespace

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
