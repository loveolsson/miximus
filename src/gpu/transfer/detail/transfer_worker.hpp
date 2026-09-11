#pragma once

#include "gpu/device.hpp"

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

    std::thread        worker_;
    std::thread        resource_worker_;
    const size_t       memory_budget_;
    std::atomic_size_t memory_usage_{};

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
    device_s&           device_;
    recording_context_s recording_context_;
    transfer_worker_s(device_s& device, size_t memory_budget)
        : memory_budget_(memory_budget)
        , device_(device)
        , recording_context_(device.create_recording_context())
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

    template <typename Stream>
    void initialize_conversion_texture(Stream& stream)
    {
        if (!stream.config.conversion_sampling || stream.conversion_texture) {
            return;
        }
        const auto dimensions = stream.transfer_plan.host_layout.image_dimensions;
        const auto sampling   = *stream.config.conversion_sampling;
        const auto bytes      = texture_s::estimate_storage_byte_size(dimensions, format_e::rgba_unorm16, sampling);
        if (!reserve_memory(bytes)) {
            throw std::bad_alloc();
        }
        try {
            stream.conversion_texture = std::make_shared<texture_s>(
                device_, dimensions, format_e::rgba_unorm16, channel_order_e::rgba, sampling);
            stream.conversion_reserved_bytes = bytes;
        } catch (...) {
            release_memory(bytes);
            throw;
        }
    }

    template <typename Stream>
    void release_conversion_texture(Stream& stream)
    {
        stream.conversion_texture.reset();
        release_memory(std::exchange(stream.conversion_reserved_bytes, 0));
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

    size_t memory_usage() const noexcept { return memory_usage_.load(std::memory_order_relaxed); }
    size_t memory_budget() const noexcept { return memory_budget_; }
};

} // namespace miximus::gpu::transfer::detail
