#include "shutdown_signal.hpp"

#include <Windows.h>
#include <atomic>
#include <csignal>
#include <stdexcept>
#include <system_error>

namespace miximus::utils {
namespace {

static_assert(std::atomic_bool::is_always_lock_free);

auto& get_shutdown_request() noexcept
{
    static std::atomic_bool requested{};
    return requested;
}

void handle_signal(int /*signal*/) noexcept { get_shutdown_request().store(true, std::memory_order_relaxed); }

BOOL WINAPI handle_console_control(DWORD event) noexcept
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }

    // Windows invokes this on a separate thread. Teardown stays on the main thread.
    get_shutdown_request().store(true, std::memory_order_relaxed);
    return TRUE;
}

} // namespace

void install_shutdown_signal_handlers()
{
    if (std::signal(SIGINT, handle_signal) == SIG_ERR || std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        throw std::runtime_error("Cannot install application shutdown signal handlers");
    }

    // The last registered console handler runs first. CEF installs a handler
    // that ends the process, bypassing the host's configuration save and teardown.
    static bool installed = false;
    if (installed) {
        if (SetConsoleCtrlHandler(handle_console_control, FALSE) == FALSE) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(), "Cannot remove application console handler");
        }
        installed = false;
    }

    if (SetConsoleCtrlHandler(handle_console_control, TRUE) == FALSE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "Cannot install application console handler");
    }
    installed = true;
}

bool shutdown_requested() noexcept { return get_shutdown_request().load(std::memory_order_relaxed); }

} // namespace miximus::utils
