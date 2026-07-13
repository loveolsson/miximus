#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/configuration.hpp"
#include "core/node_manager.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "utils/thread_priority.hpp"
#include "web_server/server.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "Shutdown timed out, forcing exit\n";
        std::_Exit(1);
    }).detach();
}

} // namespace

int main(int argc, char* argv[])
{
    (void)std::signal(SIGINT, signal_handler);
    (void)std::signal(SIGTERM, signal_handler);

    auto log_level     = spdlog::level::info;
    auto settings_path = path(argv[0]).parent_path() / "settings.json";

    {
        const std::vector<std::string_view> arguments(argv + 1, argv + argc);

        for (size_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == "--log-debug") {
                log_level = spdlog::level::debug;
            } else if (arguments[i] == "--log-trace") {
                log_level = spdlog::level::trace;
            } else if (arguments[i] == "--settings" && i + 1 < arguments.size()) {
                settings_path = arguments[++i];
            }
        }
    }

    logger::init_loggers(log_level);
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

            app.frame_info.timestamp = utils::flicks_now();
            app.frame_info.pts       = utils::flicks{0};
            app.frame_info.duration  = utils::k_flicks_one_second / 60;

            while (get_signal_status() == 0) {
                // getlog("app")->info("Frame no {}", frame_no++);

                node_manager.tick_one_frame(&app);

                gpu::context_s::poll();

                app.frame_info.timestamp += app.frame_info.duration;
                app.frame_info.pts += app.frame_info.duration;
                app.frame_info.field_even = !app.frame_info.field_even;

                auto now = utils::flicks_now();
                if (app.frame_info.timestamp + app.frame_info.duration < now) {
                    getlog("app")->info("Late frame");
                    app.frame_info.timestamp += app.frame_info.duration;
                }

                if (app.frame_info.timestamp > now) {
                    std::this_thread::sleep_for(app.frame_info.timestamp - now);
                }
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
