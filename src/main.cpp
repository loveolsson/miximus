#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/application_settings.hpp"
#include "core/clock_source.hpp"
#include "core/configuration.hpp"
#include "core/frame_scheduler.hpp"
#include "core/node_manager.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "utils/process_id.hpp"
#include "utils/thread_priority.hpp"
#include "web_server/server.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace miximus;
using namespace std::filesystem;
using namespace std::chrono_literals;
namespace {

constexpr int HTTP_PORT = 7351;

auto& get_signal_status()
{
    static volatile std::sig_atomic_t signal_status = 0;
    return signal_status;
}

void signal_handler(int /*signal*/) { get_signal_status() = 1; }

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
    auto        writer  = app->status_registry()->write_node(core::APPLICATION_SETTINGS_ID);
    writer.write("clock_source", std::string(scheduler.clock_name()));
    writer.write("frame_number", context.frame_number);
    writer.write("pts_flicks", context.pts.count());
    writer.write("render_duration_us", to_microseconds(metrics.render_duration));
    writer.write("start_lateness_us", to_microseconds(metrics.start_lateness));
    writer.write("deadline_margin_us", to_microseconds(metrics.deadline_margin));
    writer.write("skipped_frames_last", metrics.skipped_frames);
    writer.write("skipped_frames_total", metrics.skipped_frames_total);
    writer.write("sustained_overload", metrics.sustained_overload);
}

} // namespace

int main(int argc, char* argv[])
{
    (void)std::signal(SIGINT, signal_handler);
    (void)std::signal(SIGTERM, signal_handler);

    auto                                         log_level     = spdlog::level::info;
    auto                                         settings_path = path(argv[0]).parent_path() / "settings.json";
    std::optional<std::chrono::duration<double>> stop_after;

    {
        const std::vector<std::string_view> arguments(argv + 1, argv + argc);

        for (size_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == "--log-debug") {
                log_level = spdlog::level::debug;
            } else if (arguments[i] == "--log-trace") {
                log_level = spdlog::level::trace;
            } else if (arguments[i] == "--settings" && i + 1 < arguments.size()) {
                settings_path = arguments[++i];
            } else if (arguments[i] == "--stop-after" && i + 1 < arguments.size()) {
                double     seconds{};
                const auto value           = arguments[++i];
                const auto [end, error]    = std::from_chars(value.data(), value.data() + value.size(), seconds);
                const bool parsed_entirely = error == std::errc{} && end == value.data() + value.size();
                if (!parsed_entirely || seconds <= 0.0) {
                    std::cerr << "--stop-after requires a positive number of seconds\n";
                    return EXIT_FAILURE;
                }
                stop_after = std::chrono::duration<double>{seconds};
            }
        }
    }

    logger::init_loggers(log_level);
    getlog("app")->info("Process ID: {}", utils::process_id());
    utils::set_max_thread_priority();

    try {
        {
            core::app_state_s app;
            // web_server declared AFTER app so it is destroyed BEFORE app — the
            // websocketpp endpoint holds a raw pointer to cfg_executor_ and must
            // not outlive it.
            auto web_server = web_server::create_web_server();
            web_server->start(HTTP_PORT, app.cfg_executor());

            core::node_manager_s  node_manager;
            core::configuration_s configuration(node_manager);
            configuration.load_file(settings_path);

            // Set up web server config getters
            web_server->set_config_getters({
                .node_config = std::bind_front(&core::configuration_s::get_snapshot, &configuration),
            });

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager.add_adapter(std::make_unique<core::websocket_config_s>(
                node_manager, configuration, *web_server, *app.font_registry()));

            core::steady_clock_source_s frame_clock;
            core::frame_scheduler_s     frame_scheduler(frame_clock);

            std::optional<std::chrono::steady_clock::time_point> stop_time;
            if (stop_after.has_value()) {
                stop_time = std::chrono::steady_clock::now() +
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(*stop_after);
            }

            uint64_t      status_epoch{};
            utils::flicks next_status_pts{};

            while (get_signal_status() == 0 &&
                   (!stop_time.has_value() || std::chrono::steady_clock::now() < *stop_time)) {
                node_manager.tick_one_frame(&app, frame_scheduler);

                gpu::context_s::poll();

                const auto& metrics = frame_scheduler.finish_frame();
                const auto& context = app.frame_context();
                if (context.epoch != status_epoch || context.pts >= next_status_pts) {
                    publish_scheduler_status(&app, frame_scheduler, metrics);
                    status_epoch    = context.epoch;
                    next_status_pts = context.pts + utils::k_flicks_one_second;
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
                configuration.save_file(settings_path);
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
