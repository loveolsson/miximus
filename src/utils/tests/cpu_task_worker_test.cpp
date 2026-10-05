#include "utils/cpu_task_worker.hpp"

#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <stdexcept>
#include <vector>

namespace miximus::utils { namespace {

template <typename T>
T require_task(std::optional<T> task)
{
    if (!task.has_value()) {
        throw std::runtime_error("Worker rejected a test task before shutdown");
    }
    return std::move(*task);
}

TEST(CpuTaskWorker, OwnsMoveOnlyInputsAndPropagatesFailures)
{
    cpu_task_worker_s worker;
    auto              result = require_task(worker.submit(
        cpu_task_priority_e::normal, [](std::unique_ptr<int> value) { return *value; }, std::make_unique<int>(42)));
    EXPECT_EQ(result.get(), 42);
    auto failure = require_task(
        worker.submit(cpu_task_priority_e::normal, []() -> int { throw std::runtime_error("render failure"); }));
    EXPECT_THROW(failure.get(), std::runtime_error);
    auto next = require_task(worker.submit(cpu_task_priority_e::normal, [] { return 7; }));
    EXPECT_EQ(next.get(), 7);
}

TEST(CpuTaskWorker, PreservesMoveOnlyAndReferenceResults)
{
    cpu_task_worker_s worker;
    auto owned = require_task(worker.submit(cpu_task_priority_e::normal, [] { return std::make_unique<int>(42); }));
    EXPECT_EQ(*owned.get(), 42);
    int  value     = 7;
    auto reference = require_task(worker.submit(cpu_task_priority_e::normal, [&]() -> int& { return value; }));
    EXPECT_EQ(&reference.get(), &value);
}

TEST(CpuTaskWorker, AbandonedResultDoesNotWaitForWork)
{
    cpu_task_worker_s  worker;
    std::promise<void> release;
    auto               gate = release.get_future();
    auto               result =
        require_task(worker.submit(cpu_task_priority_e::normal, [gate = std::move(gate)]() mutable { gate.wait(); }));
    // A future from std::async would block here, preventing release.
    result = {};
    release.set_value();
}

// Destruction always releases the active task, including after a failed assertion.
struct blocked_worker_s
{
    std::promise<void> started;
    std::latch         release{1};
    cpu_task_worker_s  worker;
    bool               released{};

    blocked_worker_s()
    {
        (void)worker.submit(cpu_task_priority_e::normal, [this] {
            started.set_value();
            release.wait();
        });
        started.get_future().wait();
    }
    blocked_worker_s(const blocked_worker_s&)            = delete;
    blocked_worker_s& operator=(const blocked_worker_s&) = delete;
    blocked_worker_s(blocked_worker_s&&)                 = delete;
    blocked_worker_s& operator=(blocked_worker_s&&)      = delete;

    void unblock()
    {
        if (!released) {
            released = true;
            release.count_down();
        }
    }
    ~blocked_worker_s() { unblock(); }
};

TEST(CpuTaskWorker, StartsHigherPrioritiesFirstAndPreservesTies)
{
    std::vector<int> order;
    {
        blocked_worker_s blocked;
        const std::array priorities{cpu_task_priority_e::normal,
                                    cpu_task_priority_e::interactive,
                                    cpu_task_priority_e::background,
                                    cpu_task_priority_e::interactive,
                                    cpu_task_priority_e::normal,
                                    cpu_task_priority_e::interactive};
        for (int i = 0; i < 6; ++i) {
            EXPECT_TRUE(blocked.worker.submit(priorities.at(i), [&, i] { order.push_back(i); }));
        }
        auto done = require_task(blocked.worker.submit(cpu_task_priority_e::background, [] {}));
        blocked.unblock();
        done.get();
    }
    EXPECT_EQ(order, (std::vector<int>{1, 3, 5, 0, 4, 2}));
}

TEST(CpuTaskWorker, CancelsQueuedWorkAndReleasesInputs)
{
    blocked_worker_s   blocked;
    auto               input     = std::make_shared<int>(42);
    std::weak_ptr<int> observed  = input;
    auto               cancelled = require_task(
        blocked.worker.submit(cpu_task_priority_e::interactive, [input = std::move(input)] { return *input; }));
    EXPECT_TRUE(cancelled.cancel());
    EXPECT_FALSE(cancelled.cancel());
    EXPECT_TRUE(observed.expired());
    EXPECT_EQ(cancelled.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_THROW(cancelled.get(), cpu_task_cancelled_s);
    EXPECT_TRUE(blocked.worker.submit(cpu_task_priority_e::normal, [] {}));
}

TEST(CpuTaskWorker, CannotCancelRunningOrCompletedWork)
{
    cpu_task_worker_s  worker;
    std::promise<void> started;
    std::promise<void> release;
    auto               active = require_task(worker.submit(cpu_task_priority_e::normal, [&] {
        started.set_value();
        release.get_future().wait();
        return 42;
    }));
    started.get_future().wait();
    EXPECT_FALSE(active.cancel());
    release.set_value();
    EXPECT_EQ(active.get(), 42);
    EXPECT_FALSE(active.cancel());
    cpu_task_s<int> empty;
    EXPECT_FALSE(empty.cancel());
}

TEST(CpuTaskWorker, MovedHandleAndResultCanOutliveWorker)
{
    cpu_task_s<int> result;
    {
        blocked_worker_s blocked;
        auto             pending = require_task(blocked.worker.submit(cpu_task_priority_e::normal, [] { return 42; }));
        result                   = std::move(pending);
        // Querying an empty moved handle is supported.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_FALSE(pending.valid());
        // Cancellation of a moved handle is a no-op.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_FALSE(pending.cancel());
        blocked.unblock();
        EXPECT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    }
    EXPECT_FALSE(result.cancel());
    EXPECT_EQ(result.get(), 42);
}

TEST(CpuTaskWorker, AcceptsMoreThanTheFormerQueueLimit)
{
    blocked_worker_s             blocked;
    std::vector<cpu_task_s<int>> tasks;
    for (int i = 0; i < 1024; ++i) {
        auto task = require_task(blocked.worker.submit(cpu_task_priority_e::normal, [i] { return i; }));
        tasks.push_back(std::move(task));
    }
    blocked.unblock();
    for (int i = 0; i < 1024; ++i) {
        EXPECT_EQ(tasks[i].get(), i);
    }
}

TEST(CpuTaskWorker, ShutdownDiscardsQueuedWorkAndReleasesInputs)
{
    cpu_task_s<int>    discarded;
    std::atomic<int>   calls{};
    auto               input    = std::make_shared<int>(42);
    std::weak_ptr<int> observed = input;
    {
        blocked_worker_s blocked;
        auto             queued =
            require_task(blocked.worker.submit(cpu_task_priority_e::interactive, [input = std::move(input), &calls] {
                ++calls;
                return *input;
            }));
        discarded = std::move(queued);
        // Stop while the active task is still blocked: queued work must be
        // discarded immediately and never run when the active task returns.
        blocked.worker.request_stop();
        EXPECT_TRUE(observed.expired());
        EXPECT_EQ(discarded.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        EXPECT_FALSE(blocked.worker.submit(cpu_task_priority_e::normal, [] {}));
        blocked.worker.request_stop(); // Idempotent, including in the destructor.
    }
    EXPECT_EQ(calls.load(), 0);
    EXPECT_FALSE(discarded.cancel());
    EXPECT_THROW(discarded.get(), cpu_task_cancelled_s);
}

TEST(CpuTaskWorker, CancellationRacingStartHasExactlyOneOutcome)
{
    cpu_task_worker_s worker;
    for (int i = 0; i < 200; ++i) {
        std::atomic<int> calls{};
        auto             task = require_task(worker.submit(cpu_task_priority_e::normal, [&] { ++calls; }));
        if (task.cancel()) {
            EXPECT_THROW(task.get(), cpu_task_cancelled_s);
            EXPECT_EQ(calls.load(), 0);
        } else {
            task.get();
            EXPECT_EQ(calls.load(), 1);
        }
    }
}

}} // namespace miximus::utils
