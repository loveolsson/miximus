#include "shutdown_watchdog.hpp"

#include "failure_shutdown.hpp"
#include "filesystem.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace miximus::utils {
namespace {
using namespace std::chrono_literals;
constexpr auto NO_PROGRESS_TIMEOUT = std::chrono::seconds{MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS};

int64_t now_ms() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct shutdown_state_s
{
    std::atomic_bool       failure_claimed;
    std::atomic_bool       failed;
    std::atomic_bool       parked;
    bool                   frozen{};
    std::mutex             snapshot_mutex;
    std::atomic_bool       recovery_finished;
    std::array<char, 1024> reason{};
    std::atomic<int64_t>   deadline;
    std::atomic<int64_t>   timeout{std::chrono::duration_cast<std::chrono::milliseconds>(NO_PROGRESS_TIMEOUT).count()};
    std::atomic<std::shared_ptr<const recovery_settings_s>> settings;
    std::mutex                                              step_mutex;
    std::string                                             step{"shutdown startup"};
    std::jthread                                            watchdog;
    std::jthread                                            recovery;

    void request(std::string_view message, std::string_view detail = {}) noexcept
    {
        if (!failure_claimed.exchange(true)) {
            const auto count = std::min(message.size(), reason.size() - 1);
            std::copy_n(message.begin(), count, reason.begin());
            if (!detail.empty() && count + 2 < reason.size() - 1) {
                reason.at(count)     = ':';
                reason.at(count + 1) = ' ';
                std::copy_n(detail.begin(),
                            std::min(detail.size(), reason.size() - count - 3),
                            reason.begin() + static_cast<std::ptrdiff_t>(count + 2));
            }
            failed.store(true, std::memory_order_release);
        }
    }

    void watch(const std::stop_token& stop)
    {
        while (!stop.stop_requested()) {
            if (failed.load(std::memory_order_acquire)) {
                int64_t expected = 0;
                deadline.compare_exchange_strong(expected, now_ms() + timeout.load());
            }
            const auto limit = deadline.load();
            if (limit != 0 && now_ms() >= limit) {
                auto expected = limit;
                if (!deadline.compare_exchange_strong(expected, -1)) {
                    continue;
                }
                request("Shutdown watchdog timed out; recovery settings will be saved if available");
                // The recovery worker is independent: a blocked filesystem/logger
                // must never prevent this last-resort exit.
                std::this_thread::sleep_for(1s);
                std::_Exit(EXIT_FAILURE);
            }
            std::this_thread::sleep_for(10ms);
        }
    }

    void recover(const std::stop_token& stop)
    {
        while (!stop.stop_requested() && !failed.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(10ms);
        }
        if (!failed.load(std::memory_order_acquire)) {
            return;
        }
        // Save before reporting: even diagnostic output can block.
        try {
            if (const auto snapshot = settings.load()) {
                std::filesystem::path path;
                try {
                    path = write_recovery_settings(*snapshot);
                } catch (...) {
                    // A read-only/unavailable settings directory must not prevent recovery.
                    const recovery_settings_s fallback{
                        .path     = std::filesystem::temp_directory_path() / snapshot->path.filename(),
                        .contents = snapshot->contents,
                    };
                    path = write_recovery_settings(fallback);
                }
                std::fprintf(stderr, "Recovery settings saved to %s\n", path_to_utf8(path).c_str());
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Recovery settings could not be saved: %s\n", error.what());
        } catch (...) {
            std::fputs("Recovery settings could not be saved\n", stderr);
        }
        std::fprintf(stderr, "Failure shutdown requested: %s\n", reason.data());
        std::fflush(stderr);
        recovery_finished.store(true);
    }

    shutdown_state_s()
    {
        watchdog = std::jthread([this](const std::stop_token& stop) { watch(stop); });
        recovery = std::jthread([this](const std::stop_token& stop) { recover(stop); });
    }
};

shutdown_state_s& state()
{
    static shutdown_state_s instance;
    return instance;
}

shutdown_state_s& failure_state() noexcept
{
    try {
        return state();
    } catch (...) {
        // Normal startup initializes the monitor before installing the terminate
        // handler. If an earlier failure cannot even construct it, no watchdog or
        // recovery worker exists; exit without unwinding the caller's resources.
        std::_Exit(EXIT_FAILURE);
    }
}

void handle_unexpected_termination() noexcept { fail_without_unwinding("Unhandled exception or noexcept violation"); }

} // namespace

void initialize_shutdown_monitor()
{
    (void)state();
    std::set_terminate(handle_unexpected_termination);
}

void request_failure_shutdown(std::string_view message, std::string_view detail) noexcept
{
    failure_state().request(message, detail);
}
bool failure_shutdown_requested() noexcept { return failure_state().failed.load(std::memory_order_acquire); }

[[noreturn]] void fail_without_unwinding(std::string_view message, std::string_view detail) noexcept
{
    auto& monitor = failure_state();
    monitor.parked.store(true);
    monitor.request(message, detail);
    for (;;) {
        std::this_thread::sleep_for(1s);
    }
}

void publish_recovery_settings(std::filesystem::path path, std::string contents)
{
    auto&                  monitor  = state();
    auto                   snapshot = std::make_shared<const recovery_settings_s>(std::move(path), std::move(contents));
    const std::scoped_lock lock(monitor.snapshot_mutex);
    if (!monitor.frozen) {
        monitor.settings.store(std::move(snapshot));
    }
}
std::shared_ptr<const recovery_settings_s> recovery_settings() { return state().settings.load(); }
void                                       freeze_recovery_settings()
{
    auto&                  monitor = state();
    const std::scoped_lock lock(monitor.snapshot_mutex);
    monitor.frozen = true;
}

void start_shutdown_watchdog(std::chrono::seconds timeout)
{
    auto&      monitor = state();
    const auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout > 0s ? timeout : NO_PROGRESS_TIMEOUT).count();
    int64_t expected = 0;
    if (monitor.deadline.compare_exchange_strong(expected, now_ms() + duration)) {
        monitor.timeout.store(duration);
    }
}

void begin_shutdown_step(std::string step_info)
{
    auto& monitor = state();
    {
        const std::scoped_lock lock(monitor.step_mutex);
        monitor.step = step_info;
    }
    if (const auto log = getlog("app")) {
        log->info("Shutdown starting: {}", step_info);
    }
}

void report_shutdown_step_completed()
{
    auto& monitor = state();
    if (monitor.deadline.load() != 0) {
        monitor.deadline.store(now_ms() + monitor.timeout.load());
    }
    std::string completed_step;
    {
        const std::scoped_lock lock(monitor.step_mutex);
        completed_step = monitor.step;
    }
    if (const auto log = getlog("app")) {
        log->info("Shutdown completed: {}", completed_step);
    }
}

void finish_shutdown_watchdog()
{
    auto& monitor = state();
    if (monitor.failed.load()) {
        while (!monitor.recovery_finished.load() || monitor.parked.load()) {
            std::this_thread::sleep_for(10ms);
        }
    }
    monitor.timeout.store(std::chrono::duration_cast<std::chrono::milliseconds>(NO_PROGRESS_TIMEOUT).count());
    monitor.deadline.store(0);
}

} // namespace miximus::utils
