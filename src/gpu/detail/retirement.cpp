#include "retirement.hpp"

#include <utility>

namespace miximus::gpu::detail {

void retirement_queue_s::retire(uint64_t after, std::function<void()> destroy)
{
    if (after == 0) {
        destroy();
        return;
    }

    const std::scoped_lock guard(mutex_);
    pending_.push_back({after, std::move(destroy)});
}

void retirement_queue_s::collect(uint64_t completed)
{
    std::vector<entry_s> ready;
    {
        const std::scoped_lock guard(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->completion <= completed) {
                ready.push_back(std::move(*it));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& item : ready) {
        item.destroy();
    }
}

void retirement_queue_s::drain()
{
    for (auto& item : pending_) {
        item.destroy();
    }
    pending_.clear();
}
} // namespace miximus::gpu::detail
