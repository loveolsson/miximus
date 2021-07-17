#include "core/adapters/adapter_websocket.hpp"
#include "core/app_state.hpp"
#include "core/node_manager.hpp"
#include "logger/logger.hpp"
#include "nodes/decklink/decklink.hpp"
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

static volatile std::sig_atomic_t g_signal_status = 0;

static void signal_handler(int /*signal*/) { g_signal_status = 1; }

static void load_settings(core::node_manager_s& manager, const path& settings_path)
{
    auto log = spdlog::get("app");

    std::ifstream file(settings_path);
    if (file.is_open()) {
        log->info("Reading settings from {}", settings_path.u8string());

        try {
            manager.set_config(json::parse(file));
        } catch (json::exception& e) {
            // This error should panic as we don't want to run the app with a partial config, or overwrite
            // the broken file with an empty config on exit
            throw std::runtime_error(std::string("Failed to parse settings file: ") + e.what());
        }
    } else {
        log->error("Failed to open settings file {}", settings_path.u8string());
    }
}

static void save_settings(core::node_manager_s& manager, const path& settings_path)
{
    auto log = spdlog::get("app");

    std::ofstream file(settings_path);
    if (file.is_open()) {
        log->info("Writing settings to {}", settings_path.u8string());
        file << std::setfill(' ') << std::setw(2) << manager.get_config();
    } else {
        log->error("Failed to write settings to {}", settings_path.u8string());
    }
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);

    try {
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

        nodes::decklink::log_device_names();

        {
            core::app_state_s    app;
            web_server::server_s web_server;
            core::node_manager_s node_manager;

            load_settings(node_manager, settings_path);

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager.add_adapter(std::make_unique<core::websocket_config_s>(node_manager, web_server));
            web_server.start(7351);

            while (g_signal_status == 0) {
                gpu::context::poll();
                node_manager.tick_one_frame(app);
                std::this_thread::sleep_for(16ms);
            }

            web_server.stop();
            node_manager.clear_adapters();

            save_settings(node_manager, settings_path);

            spdlog::get("app")->info("Exiting...");
        }

        spdlog::shutdown();
    } catch (std::exception& e) {
        std::cout << std::endl << "Panic: " << e.what() << std::endl;
    }

    return 0;
}