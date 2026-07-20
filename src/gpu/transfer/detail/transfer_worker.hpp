#pragma once

#include "gpu/context.hpp"

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

// Owns the mechanics shared by upload and download services. Derived keeps the
// direction-specific task state machine; returning false from process_task()
// retains a task for a later retry on this worker.
template <typename Derived, typename Task>
class transfer_worker_s : public std::enable_shared_from_this<Derived>
{
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Task>        tasks_;
    bool                    stopping_{};

    std::unique_ptr<context_s> context_;
    std::thread                worker_;
    const size_t               memory_budget_;
    std::atomic_size_t         memory_usage_{};

    void run()
    {
        const context_scope_s context_scope(*context_);
        std::deque<Task>      delayed;

        while (true) {
            Task task{};
            {
                std::unique_lock lock(queue_mutex_);
                if (tasks_.empty() && !stopping_) {
                    if (delayed.empty()) {
                        queue_cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                    } else {
                        queue_cv_.wait_for(
                            lock, std::chrono::milliseconds(1), [this] { return stopping_ || !tasks_.empty(); });
                    }
                }
                if (stopping_ && tasks_.empty() && delayed.empty()) {
                    break;
                }
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                } else if (!delayed.empty()) {
                    task = std::move(delayed.front());
                    delayed.pop_front();
                } else {
                    continue;
                }
            }

            if (!static_cast<Derived*>(this)->process_task(task)) {
                delayed.emplace_back(std::move(task));
            }
        }
    }

  protected:
    transfer_worker_s(context_s* parent, size_t memory_budget)
        : context_(context_s::create_unique_context(false, parent))
        , memory_budget_(memory_budget)
    {
    }

    bool reserve_memory(size_t bytes)
    {
        auto current = memory_usage_.load(std::memory_order_relaxed);
        while (bytes <= memory_budget_ && current <= memory_budget_ - bytes) {
            if (memory_usage_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release_memory(size_t bytes) { memory_usage_.fetch_sub(bytes, std::memory_order_relaxed); }

    bool resize_memory_reservation(size_t old_size, size_t new_size)
    {
        if (new_size > old_size) {
            return reserve_memory(new_size - old_size);
        }
        release_memory(old_size - new_size);
        return true;
    }

  public:
    void start()
    {
        auto self = this->shared_from_this();
        worker_   = std::thread([self = std::move(self)] { self->run(); });
    }

    void enqueue(Task task)
    {
        {
            const std::scoped_lock lock(queue_mutex_);
            if (stopping_) {
                return;
            }
            tasks_.emplace_back(std::move(task));
        }
        queue_cv_.notify_one();
    }

    void stop()
    {
        {
            const std::scoped_lock lock(queue_mutex_);
            stopping_ = true;
        }
        queue_cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    size_t memory_usage() const { return memory_usage_.load(std::memory_order_relaxed); }
    size_t memory_budget() const { return memory_budget_; }
};

} // namespace miximus::gpu::transfer::detail
