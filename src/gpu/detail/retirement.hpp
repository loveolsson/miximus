#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace miximus::gpu::detail {

class retirement_queue_s
{
    struct entry_s
    {
        uint64_t              completion;
        std::function<void()> destroy;
    };
    std::mutex           mutex_;
    std::vector<entry_s> pending_;

  public:
    void retire(uint64_t after, std::function<void()> destroy);
    void collect(uint64_t completed);
    // Device teardown only, after native idle and submission-worker shutdown.
    void drain();
};
} // namespace miximus::gpu::detail
