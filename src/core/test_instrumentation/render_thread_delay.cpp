#include "render_thread_delay.hpp"

#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "logger/logger.hpp"
#include "nodes/system/register.hpp"
#include "types/node_status_json.hpp"

#include <thread>

namespace miximus::core::test_instrumentation {

render_thread_delay_test_s::render_thread_delay_test_s(const app_state_s& app)
{
    const auto& options = app.command_line_options().render_thread_delay_test;
    if (!options.has_value()) {
        return;
    }

    delay_        = options->delay;
    every_frames_ = options->every_frames;
    getlog("app")->warn("Test instrumentation will stall the render thread for {} ms every {} rendered frames",
                        delay_->count(),
                        every_frames_);
}

void render_thread_delay_test_s::inject_before_render_frame()
{
    if (delay_.has_value() && rendered_frames_ != 0 && rendered_frames_ % every_frames_ == 0) {
        ++injections_;
        std::this_thread::sleep_for(*delay_);
    }
    ++rendered_frames_;
}

void render_thread_delay_test_s::publish_status(app_state_s* app) const
{
    if (!delay_.has_value()) {
        return;
    }

    app->status_registry()->write(nodes::system::SETTINGS_NODE_ID,
                                  status::render_delay_test_status_s{
                                      .test_render_delay_ms         = delay_->count(),
                                      .test_render_delay_every      = every_frames_,
                                      .test_render_delay_injections = injections_,
                                  });
}

} // namespace miximus::core::test_instrumentation
