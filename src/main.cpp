#include "application/app_state.hpp"
#include "logger/logger.hpp"
#include "nodes/config/adapter_websocket.hpp"
#include "nodes/config/manager.hpp"
#include "nodes/decklink/decklink.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

static volatile std::sig_atomic_t g_signal_status = 0;

void signal_handler(int /*signal*/) { g_signal_status = 1; }

int main(int argc, char** argv)
{
    using namespace miximus;
    using namespace std::filesystem;
    using nlohmann::json;
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
        auto log = spdlog::get("app");

        {
            auto names = nodes::decklink::get_device_names();
            log->info("Found {} DeckLink device(s)", names.size());
            for (auto& name : names) {
                log->info(" -- \"{}\"", name);
            }
        }

        {
            web_server::server  web_server_;
            nodes::node_manager node_manager_;

            {
                log->info(R"(Reading settings file "{}" )", settings_path.u8string());
                std::ifstream file(settings_path);
                if (file.is_open()) {
                    try {
                        node_manager_.set_config(json::parse(file));
                    } catch (json::exception& e) {
                        // This error should panic as we don't want to run the app with a partial config, or overwrite
                        // the broken file with an empty config on exit
                        throw std::runtime_error(std::string("Failed to parse settings file: ") + e.what());
                    }
                } else {
                    log->error("Failed to open settings file {}", settings_path.u8string());
                }
            }

            // Add adapters _after_ config is loaded to prevent spam to the adapters during load
            node_manager_.add_adapter(std::make_unique<nodes::websocket_config>(node_manager_, web_server_));

            {
                application::state app;

                web_server_.start(7351);

                while (g_signal_status == 0) {
                    gpu::context::poll();
                    node_manager_.run_one_frame();
                    std::this_thread::sleep_for(16ms);
                }
            }

            node_manager_.clear_adapters();
            web_server_.stop();

            {
                std::ofstream file(settings_path);
                if (file.is_open()) {
                    log->info("Writing settings to {}", settings_path.u8string());
                    file << std::setfill(' ') << std::setw(2) << node_manager_.get_config();
                } else {
                    log->error("Failed to write settings to {}", settings_path.u8string());
                }
            }

            gpu::context::terminate();

            log->info("Exiting...");
        }

        spdlog::shutdown();
    } catch (std::exception& e) {
        std::cout << std::endl << "Panic: " << e.what() << std::endl;
    }

    return 0;
}