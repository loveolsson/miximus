#include "shutdown_signal.hpp"

#include <csignal>
#include <stdexcept>

namespace miximus::utils {
namespace {

auto& get_shutdown_request() noexcept
{
    static volatile std::sig_atomic_t requested = 0;
    return requested;
}

void handle_signal(int /*signal*/) noexcept { get_shutdown_request() = 1; }

} // namespace

void install_shutdown_signal_handlers()
{
    if (std::signal(SIGINT, handle_signal) == SIG_ERR || std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        throw std::runtime_error("Cannot install application shutdown signal handlers");
    }
}

bool shutdown_requested() noexcept { return get_shutdown_request() != 0; }

} // namespace miximus::utils
