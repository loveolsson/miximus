#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace miximus::utils {

// Lowest to highest priority. Extend or reorder this list to change scheduling
// centrally; these values are internal and are not persisted or sent over the wire.
enum class cpu_task_priority_e
{
    background,
    normal,
    interactive,
};

class cpu_task_cancelled_s : public std::exception
{
  public:
    const char* what() const noexcept override { return "CPU task cancelled before starting"; }
};

namespace detail {
struct cpu_task_entry_s
{
    cpu_task_priority_e            priority;
    std::packaged_task<void(bool)> invoke;
};

struct cpu_task_queue_s
{
    std::mutex                                    mutex;
    std::condition_variable                       condition;
    std::deque<std::shared_ptr<cpu_task_entry_s>> tasks;
    bool                                          closing{};
};
} // namespace detail

// Move-only result and cancellation handle. Dropping it does not cancel work.
// Handles may outlive the worker; cancellation never interrupts a running task.
template <typename T>
class cpu_task_s
{
    friend class cpu_task_worker_s;
    std::future<T>                          result_;
    std::weak_ptr<detail::cpu_task_queue_s> queue_;
    std::weak_ptr<detail::cpu_task_entry_s> entry_;

    cpu_task_s(std::future<T>                                   result,
               const std::shared_ptr<detail::cpu_task_queue_s>& queue,
               const std::shared_ptr<detail::cpu_task_entry_s>& entry)
        : result_(std::move(result))
        , queue_(queue)
        , entry_(entry)
    {
    }

  public:
    cpu_task_s()                                 = default;
    cpu_task_s(cpu_task_s&&) noexcept            = default;
    cpu_task_s& operator=(cpu_task_s&&) noexcept = default;
    cpu_task_s(const cpu_task_s&)                = delete;
    cpu_task_s& operator=(const cpu_task_s&)     = delete;

    // True only when removed before the worker claims it. Release captured
    // inputs outside the queue lock and make get() throw cpu_task_cancelled_s.
    [[nodiscard]] bool cancel()
    {
        const auto queue = queue_.lock();
        const auto entry = entry_.lock();
        if (!queue || !entry) {
            return false;
        }
        {
            const std::scoped_lock lock(queue->mutex);
            const auto             it = std::find(queue->tasks.begin(), queue->tasks.end(), entry);
            if (it == queue->tasks.end()) {
                return false;
            }
            queue->tasks.erase(it);
        }
        entry->invoke(true);
        // Release the callable even if another cancelling caller holds entry.
        entry->invoke = {};
        return true;
    }

    [[nodiscard]] bool valid() const noexcept { return result_.valid(); }
    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout) const
    {
        return result_.wait_for(timeout);
    }
    T get() { return result_.get(); }
};

// One dedicated thread for CPU-heavy tasks independent of frame boundaries,
// including image generation, text rendering, and utility or debugging work.
// Higher enum priorities start first; equal priorities keep submission order.
// There is no pending-task limit. Tasks own their inputs and never borrow
// frame-local state or node pointers. Shutdown discards queued work and joins
// only the task already running, whose dependencies must remain alive until then.
class cpu_task_worker_s
{
    std::shared_ptr<detail::cpu_task_queue_s> queue_{std::make_shared<detail::cpu_task_queue_s>()};
    std::thread                               thread_{[this] { run(); }};

    void run()
    {
        while (true) {
            std::shared_ptr<detail::cpu_task_entry_s> task;
            {
                std::unique_lock lock(queue_->mutex);
                queue_->condition.wait(lock, [this] { return queue_->closing || !queue_->tasks.empty(); });
                if (queue_->closing) {
                    return;
                }
                task = std::move(queue_->tasks.front());
                queue_->tasks.pop_front();
            }
            task->invoke(false);
        }
    }

  public:
    cpu_task_worker_s()                                    = default;
    cpu_task_worker_s(const cpu_task_worker_s&)            = delete;
    cpu_task_worker_s& operator=(const cpu_task_worker_s&) = delete;

    // Stop admission and discard all unstarted work without running its callable.
    // The active task is not preempted; destruction joins it before releasing dependencies.
    void request_stop()
    {
        std::deque<std::shared_ptr<detail::cpu_task_entry_s>> discarded;
        {
            const std::scoped_lock lock(queue_->mutex);
            queue_->closing = true;
            discarded.swap(queue_->tasks);
        }
        queue_->condition.notify_one();
        for (auto& task : discarded) {
            task->invoke(true);
            task->invoke = {};
        }
    }

    ~cpu_task_worker_s()
    {
        request_stop();
        thread_.join();
    }

    template <typename F, typename... Args>
    auto submit(cpu_task_priority_e priority, F&& function, Args&&... args)
        -> std::optional<cpu_task_s<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>
    {
        using result_t = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) -- moved arguments remain owned by the task tuple.
        auto function_with_args = [function = std::forward<F>(function),
                                   args = std::make_tuple(std::forward<Args>(args)...)]() mutable -> decltype(auto) {
            return std::apply(std::move(function), std::move(args));
        };
        // A future can retain the packaged task's state after completion. Move
        // the callable into invocation scope so its inputs are released before
        // reporting completion or cancellation, even when the result is kept.
        std::packaged_task<result_t(bool)> task([work = std::make_unique<decltype(function_with_args)>(std::move(
                                                     function_with_args))](bool cancelled) mutable -> result_t {
            auto owned_work = std::move(work);
            if (cancelled) {
                throw cpu_task_cancelled_s{};
            }
            return (*owned_work)();
        });
        auto                               future = task.get_future();
        auto                               entry  = std::make_shared<detail::cpu_task_entry_s>(detail::cpu_task_entry_s{
                                           .priority = priority,
                                           .invoke =
                std::packaged_task<void(bool)>([task = std::move(task)](bool cancelled) mutable { task(cancelled); }),
        });
        {
            const std::scoped_lock lock(queue_->mutex);
            if (queue_->closing) {
                return std::nullopt;
            }
            const auto position =
                std::find_if(queue_->tasks.begin(), queue_->tasks.end(), [priority](const auto& pending) {
                    return pending->priority < priority;
                });
            queue_->tasks.insert(position, entry);
        }
        queue_->condition.notify_one();
        return cpu_task_s<result_t>(std::move(future), queue_, entry);
    }
};

} // namespace miximus::utils
