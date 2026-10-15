#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace miximus::gpu::transfer::detail {

class memory_budget_s
{
    const size_t       memory_budget_;
    std::atomic_size_t memory_usage_{};

  public:
    explicit memory_budget_s(size_t bytes)
        : memory_budget_(bytes)
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

    size_t usage() const noexcept { return memory_usage_.load(std::memory_order_relaxed); }

    size_t limit() const noexcept { return memory_budget_; }
};

// The budget must outlive its reservations. Release only after the associated
// resources and external leases have retired, never merely after submission.
class memory_reservation_s
{
    memory_budget_s* budget_{};
    size_t           bytes_{};

  public:
    memory_reservation_s() = default;

    memory_reservation_s(memory_budget_s& budget, size_t bytes)
        : budget_(&budget)
    {
        if (!budget.reserve_memory(bytes)) {
            throw std::bad_alloc();
        }
        bytes_ = bytes;
    }

    ~memory_reservation_s() { reset(); }

    memory_reservation_s(const memory_reservation_s&)            = delete;
    memory_reservation_s& operator=(const memory_reservation_s&) = delete;

    memory_reservation_s(memory_reservation_s&& other) noexcept
        : budget_(std::exchange(other.budget_, nullptr))
        , bytes_(std::exchange(other.bytes_, 0))
    {
    }

    memory_reservation_s& operator=(memory_reservation_s&& other) noexcept
    {
        if (this != &other) {
            reset();
            budget_ = std::exchange(other.budget_, nullptr);
            bytes_  = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    void resize(size_t bytes)
    {
        if (!budget_ || !budget_->resize_memory_reservation(bytes_, bytes)) {
            throw std::bad_alloc();
        }
        bytes_ = bytes;
    }

    void reset() noexcept
    {
        if (budget_) {
            budget_->release_memory(bytes_);
            budget_ = nullptr;
            bytes_  = 0;
        }
    }
};
} // namespace miximus::gpu::transfer::detail
