#include "nodes/node_manager.hpp"
#include "web_server/web_server.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
volatile int running = 1;

namespace {
volatile std::sig_atomic_t g_signal_status = 0;
}

void signal_handler(int signal) { g_signal_status = 1; }

int main()
{
    using namespace miximus;

    node_manager           node_manager_;
    web_server::web_server web_server_;
    node_manager_.make_server_subscriptions(web_server_);

    web_server_.start(7351);

    std::signal(SIGINT, signal_handler);
    while (!g_signal_status) {
        std::this_thread::sleep_for(1ms);
    }

    std::cout << "Exiting..." << std::endl;

    return 0;
}