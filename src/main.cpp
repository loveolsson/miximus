#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/node_manager.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
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

constexpr int HTTP_PORT = 7351;

static volatile std::sig_atomic_t g_signal_status = 0;

static void signal_handler(int /*signal*/) { g_signal_status = 1; }

static void load_settings(core::node_manager_s* manager, const path& settings_path)
{
    auto log = getlog("app");

    std::ifstream file(settings_path);
    if (file.is_open()) {
        log->info("Reading settings from {}", settings_path.u8string());

        try {
            manager->set_config(json::parse(file));
        } catch (json::exception& e) {
            // This error should panic as we don't want to run the app with a partial config, or overwrite
            // the broken file with an empty config on exit
            throw std::runtime_error(std::string("Failed to parse settings file: ") + e.what());
        }
    } else {
        log->error("Failed to open settings file {}", settings_path.u8string());
    }
}

static void save_settings(core::node_manager_s* manager, const path& settings_path)
{
    auto log = getlog("app");

    std::ofstream file(settings_path);
    if (file.is_open()) {
        log->info("Writing settings to {}", settings_path.u8string());
        file << std::setfill(' ') << std::setw(2) << manager->get_config();
    } else {
        log->error("Failed to write settings to {}", settings_path.u8string());
    }
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, signal_handler);

    auto log_level     = spdlog::level::info;
    auto settings_path = path(argv[0]).parent_path() / "settings.json";

    for (int i = 1; i < argc; ++i) {
        std::string_view param(argv[i]);
        if (param == "--log-debug") {
            log_level = spdlog::level::debug;
        }

        if (param == "--settings" && i + 1 < argc) {
            settings_path = argv[++i];
        }
    }

    logger::init_loggers(log_level);

    try {
        web_server::server_s web_server;

        {
            core::app_state_s app;
            web_server.start(HTTP_PORT, app.cfg_executor());

            core::node_manager_s node_manager;
            load_settings(&node_manager, settings_path);

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager.add_adapter(std::make_unique<core::websocket_config_s>(node_manager, web_server));

            app.frame_info.pts = std::chrono::steady_clock::now();
            int64_t frame_no   = 0;

            while (g_signal_status == 0) {
                // getlog("app")->info("Frame no {}", frame_no++);

                node_manager.tick_one_frame(&app);

                gpu::context_s::poll();

                app.frame_info.pts += std::chrono::milliseconds(16);

                auto now = std::chrono::steady_clock::now();
                if (app.frame_info.pts < now) {
                    getlog("app")->info("Late frame");
                    app.frame_info.pts = now;
                } else {
                    std::this_thread::sleep_until(app.frame_info.pts);
                }

                if (frame_no++ == 500) {
                    // g_signal_status = 1;
                }
            }

            getlog("app")->info("Exiting...");

            web_server.stop();
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