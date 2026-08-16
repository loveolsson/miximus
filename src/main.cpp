#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/clock_source.hpp"
#include "core/command_line_options.hpp"
#include "core/configuration.hpp"
#include "core/frame_scheduler.hpp"
#include "core/node_manager.hpp"
#include "core/node_status_registry.hpp"
#include "core/test_instrumentation/render_thread_delay.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "nodes/system/register.hpp"
#include "types/node_status_json.hpp"
#include "utils/filesystem.hpp"
#include "utils/process_id.hpp"
#include "utils/thread_priority.hpp"
#include "web_server/server.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace miximus;
using namespace std::chrono_literals;
namespace {

constexpr int HTTP_PORT = 7351;

auto& get_signal_status() noexcept
{
    static volatile std::sig_atomic_t signal_status = 0;
    return signal_status;
}

void signal_handler(int /*signal*/) noexcept { get_signal_status() = 1; }

void start_shutdown_watchdog()
{
#ifndef MIXIMUS_SANITIZED_BUILD
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "Shutdown timed out, forcing exit\n";
        std::_Exit(1);
    }).detach();
#endif
}

void publish_scheduler_status(core::app_state_s*                     app,
                              const core::frame_scheduler_s&         scheduler,
                              const core::frame_scheduler_metrics_s& metrics)
{
    const auto to_microseconds = [](utils::flicks value) {
        return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
    };

    const auto& context = app->frame_context();
    app->status_registry()->write(nodes::system::SETTINGS_NODE_ID,
                                  status::application_scheduler_status_s{
                                      .clock_source              = std::string(scheduler.clock_name()),
                                      .frame_number              = context.frame_number,
                                      .pts_flicks                = context.program_pts.count(),
                                      .render_duration_us        = to_microseconds(metrics.render_duration),
                                      .render_duration_max_us    = to_microseconds(metrics.render_duration_max),
                                      .render_duration_max_frame = metrics.render_duration_max_frame,
                                      .start_lateness_us         = to_microseconds(metrics.start_lateness),
                                      .start_lateness_max_us     = to_microseconds(metrics.start_lateness_max),
                                      .start_lateness_max_frame  = metrics.start_lateness_max_frame,
                                      .deadline_margin_us        = to_microseconds(metrics.deadline_margin),
                                      .deadline_margin_min_us    = to_microseconds(metrics.deadline_margin_min),
                                      .deadline_margin_min_frame = metrics.deadline_margin_min_frame,
                                      .deadline_misses_total     = metrics.deadline_misses_total,
                                      .skipped_frames_last       = metrics.skipped_frames,
                                      .skipped_frames_total      = metrics.skipped_frames_total,
                                      .sustained_overload        = metrics.sustained_overload,
                                  });
}

int miximus_main(core::command_line_options_s command_line_options, std::string_view executable_name)
{
    (void)std::signal(SIGINT, signal_handler);
    (void)std::signal(SIGTERM, signal_handler);

    if (command_line_options.show_help) {
        std::cout << core::get_command_line_help(executable_name);
        return EXIT_SUCCESS;
    }

    logger::init_loggers(command_line_options.log_level);
    getlog("app")->info("Process ID: {}", utils::process_id());
    utils::set_max_thread_priority();

    try {
        {
            core::app_state_s app(std::move(command_line_options));
            // web_server declared AFTER app so it is destroyed BEFORE app — the
            // websocketpp endpoint holds a raw pointer to cfg_executor_ and must
            // not outlive it.
            auto web_server = web_server::create_web_server();
            web_server->start(HTTP_PORT, app.cfg_executor());

            core::node_manager_s  node_manager;
            core::configuration_s configuration(node_manager);
            configuration.load_file(app.command_line_options().settings_path);

            // Set up web server config getters
            web_server->set_config_getters({
                .node_config   = std::bind_front(&core::configuration_s::get_snapshot, &configuration),
                .node_statuses = [status_registry = app.status_registry()] { return status_registry->get_all(); },
                .node          = std::bind_front(&core::configuration_s::get_node, &configuration),
                .node_status   = std::bind_front(&core::configuration_s::get_node_status, &configuration),
            });

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager.add_adapter(
                core::create_websocket_adapter(node_manager, configuration, *web_server, *app.font_registry()));

            core::steady_clock_source_s                            frame_clock;
            core::frame_scheduler_s                                frame_scheduler(frame_clock);
            core::test_instrumentation::render_thread_delay_test_s render_thread_delay_test(app);

            std::optional<std::chrono::steady_clock::time_point> stop_time;
            if (app.command_line_options().stop_after.has_value()) {
                stop_time =
                    std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           *app.command_line_options().stop_after);
            }

            uint64_t      status_epoch{};
            utils::flicks next_status_pts{};

            while (get_signal_status() == 0 &&
                   (!stop_time.has_value() || std::chrono::steady_clock::now() < *stop_time)) {
                render_thread_delay_test.inject_before_render_frame();
                node_manager.tick_one_frame(&app, frame_scheduler);

                gpu::context_s::poll();

                const auto& metrics = frame_scheduler.finish_frame();
                const auto& context = app.frame_context();
                if (context.epoch != status_epoch || context.program_pts >= next_status_pts) {
                    publish_scheduler_status(&app, frame_scheduler, metrics);
                    render_thread_delay_test.publish_status(&app);
                    status_epoch    = context.epoch;
                    next_status_pts = context.program_pts + utils::k_flicks_one_second;
                }
            }

            if (stop_time.has_value() && get_signal_status() == 0) {
                getlog("app")->info("Stopping after requested runtime");
            }

            getlog("app")->info("Exiting...");
            start_shutdown_watchdog();
            web_server->stop();
            node_manager.clear_adapters();
            try {
                configuration.save_file(app.command_line_options().settings_path);
            } catch (const std::exception& error) {
                getlog("app")->error("Failed to save configuration: {}", error.what());
            }
            node_manager.clear_nodes(&app);
        }
    } catch (std::exception& e) {
        std::cout << "Panic: " << e.what() << '\n';
    }

    // gpu::context_s::terminate();
    spdlog::shutdown();
    return EXIT_SUCCESS;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    try {
        return miximus_main(core::parse_command_line_options(argc, argv),
                            utils::path_to_utf8(std::filesystem::path(argv[0])));
    } catch (const std::invalid_argument& error) {
        std::cerr << "Invalid command line: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
#else
int main(int argc, char* argv[])
{
    try {
        return miximus_main(core::parse_command_line_options(argc, argv), argv[0]);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Invalid command line: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
#endif
