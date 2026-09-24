#pragma once

#include "gpu/device.hpp"
#include "memory_budget.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace miximus::gpu::transfer::detail {

// Owns the mechanics shared by upload and readback services. Derived keeps the
// direction-specific task state machine; returning false from process_task()
// retains a task for a later retry on this worker.
template <typename Derived, typename Task>
class transfer_worker_s : public std::enable_shared_from_this<Derived>
{
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Task>        tasks_;
    // Allocation, registration and destruction must not delay transfer progress.
    std::deque<Task> resource_tasks_;
    size_t           outstanding_tasks_{};
    bool             stopping_{};

    std::thread worker_;
    std::thread resource_worker_;

    void run(std::deque<Task>& incoming)
    {
        std::deque<Task> pending;
        auto             retry_at = std::chrono::steady_clock::now();

        while (true) {
            {
                std::unique_lock lock(queue_mutex_);
                if (pending.empty()) {
                    queue_cv_.wait(lock, [this, &incoming] {
                        return !incoming.empty() || (stopping_ && outstanding_tasks_ == 0);
                    });
                } else {
                    // One polling interval for the whole pending set. Waiting
                    // once per task adds queue-depth-dependent transfer latency.
                    queue_cv_.wait_until(lock, retry_at, [&incoming] { return !incoming.empty(); });
                }

                if (stopping_ && outstanding_tasks_ == 0) {
                    break;
                }

                // Retry older work before new arrivals so a busy producer
                // cannot starve GPU completion or slot reclamation.
                while (!incoming.empty()) {
                    pending.emplace_back(std::move(incoming.front()));
                    incoming.pop_front();
                }
            }

            const auto count = pending.size();
            for (size_t i = 0; i < count; ++i) {
                auto task = std::move(pending.front());
                pending.pop_front();
                if (!static_cast<Derived*>(this)->process_task(task)) {
                    pending.emplace_back(std::move(task));
                } else {
                    {
                        const std::scoped_lock lock(queue_mutex_);
                        --outstanding_tasks_;
                    }
                    queue_cv_.notify_all();
                }
            }

            retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        }
    }

  protected:
    memory_budget_s     memory_;
    device_s&           device_;
    recording_context_s recording_context_;
    transfer_worker_s(device_s& device, size_t memory_budget)
        : memory_(memory_budget)
        , device_(device)
        , recording_context_(device.create_recording_context())
    {
    }

  public:
    void start()
    {
        auto self        = this->shared_from_this();
        worker_          = std::thread([self] { self->run(self->tasks_); });
        resource_worker_ = std::thread([self] { self->run(self->resource_tasks_); });
    }

    void enqueue(Task task)
    {
        {
            const std::scoped_lock lock(queue_mutex_);
            if (stopping_ && outstanding_tasks_ == 0) {
                return;
            }
            auto& incoming = Derived::is_resource_task(task) ? resource_tasks_ : tasks_;
            incoming.emplace_back(std::move(task));
            ++outstanding_tasks_;
        }
        queue_cv_.notify_all();
    }

    void stop()
    {
        {
            const std::scoped_lock lock(queue_mutex_);
            stopping_ = true;
        }
        queue_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (resource_worker_.joinable()) {
            resource_worker_.join();
        }
    }

    size_t memory_usage() const noexcept { return memory_.usage(); }
    size_t memory_budget() const noexcept { return memory_.limit(); }
};

} // namespace miximus::gpu::transfer::detail
