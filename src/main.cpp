#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/node_manager.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "utils/bind.hpp"
#include "utils/thread_priority.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace miximus;
using namespace std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

constexpr int HTTP_PORT = 7351;

auto& get_signal_status()
{
    static volatile std::sig_atomic_t signal_status = 0;
    return signal_status;
}

void signal_handler(int /*signal*/) { get_signal_status() = 1; }

void load_settings(core::node_manager_s* manager, const path& settings_path)
{
    auto log = getlog("app");

    std::ifstream file(settings_path);
    if (file.is_open()) {
        log->info("Reading settings from {}", settings_path.string());

        try {
            manager->set_config(json::parse(file));
        } catch (json::exception& e) {
            // This error should panic as we don't want to run the app with a partial config, or
            // overwrite the broken file with an empty config on exit
            throw std::runtime_error(std::string("Failed to parse settings file: ") + e.what());
        }
    } else {
        log->error("Failed to open settings file {}", settings_path.string());
    }
}

void save_settings(core::node_manager_s* manager, const path& settings_path)
{
    auto log = getlog("app");

    std::ofstream file(settings_path);
    if (file.is_open()) {
        log->info("Writing settings to {}", settings_path.string());
        file << std::setfill(' ') << std::setw(2) << manager->get_config();
    } else {
        log->error("Failed to write settings to {}", settings_path.string());
    }
}

} // namespace

int main(int argc, char* argv[])
{
    (void)std::signal(SIGINT, signal_handler);

    auto log_level     = spdlog::level::info;
    auto settings_path = path(argv[0]).parent_path() / "settings.json";

    for (int i = 1; i < argc; ++i) {
        const std::string_view param(argv[i]);
        if (param == "--log-debug") {
            log_level = spdlog::level::debug;
        }

        if (param == "--log-trace") {
            log_level = spdlog::level::trace;
        }

        if (param == "--settings" && i + 1 < argc) {
            settings_path = argv[++i];
        }
    }

    logger::init_loggers(log_level);
    utils::set_max_thread_priority();

    try {
        auto web_server = web_server::create_web_server();

        {
            core::app_state_s app;
            web_server->start(HTTP_PORT, app.cfg_executor());

            core::node_manager_s node_manager;
            load_settings(&node_manager, settings_path);

            // Set up web server config getters
            web_server->set_config_getters({
                .node_config = utils::bind(&core::node_manager_s::get_config, &node_manager),
            });

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager.add_adapter(std::make_unique<core::websocket_config_s>(node_manager, *web_server));

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

            web_server->stop();
            node_manager.clear_adapters();
            save_settings(&node_manager, settings_path);
            node_manager.clear_nodes(&app);
        }

    } catch (std::exception& e) {
        std::cout << "Panic: " << e.what() << std::endl;
    }

    // gpu::context_s::terminate();
    spdlog::shutdown();

    return EXIT_SUCCESS;
}