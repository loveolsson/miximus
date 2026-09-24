#pragma once

#include <atomic>
#include <cstddef>

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
} // namespace miximus::gpu::transfer::detail
