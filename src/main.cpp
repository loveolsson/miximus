#include "application/app_state.hpp"
#include "logger/logger.hpp"
#include "nodes/decklink/decklink.hpp"
#include "nodes/node_manager.hpp"
#include "web_server/web_server.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
volatile std::sig_atomic_t g_signal_status = 0;
}

void signal_handler(int signal) { g_signal_status = 1; }

int main(int argc, char* argv[])
{
    using namespace miximus;
    std::signal(SIGINT, signal_handler);

    spdlog::level::level_enum log_level = spdlog::level::info;

    for (int i = 1; i < argc; ++i) {
        std::string_view param(argv[i]);
        if (param == "--log-debug") {
            log_level = spdlog::level::debug;
        }
    }

    logger::init_loggers(log_level);
    auto log = spdlog::get("app");

    {
        auto names = nodes::decklink::get_device_names();
        log->info("Found {} DeckLink device(s):", names.size());
        for (auto& name : names) {
            log->info(" -- \"{}\"", name);
        }
    }

    {
        node_manager           node_manager_;
        web_server::web_server web_server_;
        node_manager_.make_server_subscriptions(web_server_);

        {
            application::state app;

            web_server_.start(7351);

            while (!g_signal_status) {
                gpu::context::poll();
                std::this_thread::sleep_for(1ms);
            }
        }

        gpu::context::terminate();

        log->info("Exiting...");
    }

    spdlog::shutdown();

    return 0;
}