#include "shutdown_watchdog.hpp"

#include "logger/logger.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <thread>

namespace miximus::utils {
namespace {

constexpr auto NO_PROGRESS_TIMEOUT = std::chrono::seconds{MIXIMUS_SHUTDOWN_NO_PROGRESS_TIMEOUT_SECONDS};

struct shutdown_watchdog_state_s
{
    std::mutex                            mutex;
    std::condition_variable               condition;
    std::thread                           thread;
    std::string                           current_step{"shutdown startup"};
    std::chrono::steady_clock::time_point deadline;
    uint64_t                              generation{};
    bool                                  active{};
    bool                                  finished{};
};

shutdown_watchdog_state_s& watchdog_state()
{
    static shutdown_watchdog_state_s state;
    return state;
}

void watchdog_loop()
{
    auto&            state = watchdog_state();
    std::unique_lock lock(state.mutex);
    auto             observed_generation = state.generation;

    while (!state.finished) {
        if (state.condition.wait_until(lock, state.deadline, [&state, observed_generation] {
                return state.finished || state.generation != observed_generation;
            })) {
            observed_generation = state.generation;
            continue;
        }

        const auto current_step = state.current_step;
        lock.unlock();
        const auto message = std::format("Shutdown made no progress for {} seconds during {}, forcing exit\n",
                                         NO_PROGRESS_TIMEOUT.count(),
                                         current_step);
        std::fwrite(message.data(), sizeof(char), message.size(), stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

} // namespace

void start_shutdown_watchdog()
{
    auto& state = watchdog_state();
    {
        const std::scoped_lock lock(state.mutex);
        if (state.active) {
            return;
        }
        state.active       = true;
        state.finished     = false;
        state.current_step = "shutdown startup";
        state.deadline     = std::chrono::steady_clock::now() + NO_PROGRESS_TIMEOUT;
        ++state.generation;
    }
    state.thread = std::thread(watchdog_loop);
}

void begin_shutdown_step(std::string step_info)
{
    auto& state = watchdog_state();
    {
        const std::scoped_lock lock(state.mutex);
        if (!state.active || state.finished) {
            return;
        }
        state.current_step = step_info;
    }
    getlog("app")->info("Shutdown starting: {}", step_info);
}

void report_shutdown_step_completed()
{
    auto&       state = watchdog_state();
    std::string completed_step;
    {
        const std::scoped_lock lock(state.mutex);
        if (!state.active || state.finished) {
            return;
        }
        completed_step = state.current_step;
        state.deadline = std::chrono::steady_clock::now() + NO_PROGRESS_TIMEOUT;
        ++state.generation;
    }
    state.condition.notify_one();
    getlog("app")->info("Shutdown completed: {}", completed_step);
}

void finish_shutdown_watchdog()
{
    auto& state = watchdog_state();
    {
        const std::scoped_lock lock(state.mutex);
        if (!state.active || state.finished) {
            return;
        }
        state.active   = false;
        state.finished = true;
    }
    state.condition.notify_one();
    state.thread.join();
}

} // namespace miximus::utils
