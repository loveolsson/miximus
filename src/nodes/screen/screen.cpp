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

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    std::unique_ptr<gpu::draw_state_s> draw_state_;
    input_interface_s<gpu::texture_s*> iface_tex_;
    std::unique_ptr<gpu::context_s>    context_;

  public:
    explicit node_impl() { interfaces_.emplace("tex", &iface_tex_); }

    ~node_impl() override = default;

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
                context_ = gpu::context_s::create_unique_context(true, app->ctx());
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
            if (draw_state_) {
                context_->make_current();
                draw_state_.reset();
                gpu::context_s::rewind_current();
            }
            context_.reset();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (!context_) {
            return;
        }

        auto* texture = iface_tex_.resolve_value(app, nodes, state.get_connection_set("tex"));
        auto  dim     = context_->get_framebuffer_size();

        gpu::sync_s sync;
        context_->make_current();
        sync.gpu_wait();

        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 1.0, 0, 1.0);

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

        context_->swap_buffers();
        gpu::context_s::rewind_current();
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
};

} // namespace

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
