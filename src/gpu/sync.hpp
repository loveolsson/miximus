#pragma once
#include "gpu/glad.hpp"

#include <chrono>
#include <mutex>

namespace miximus::gpu {
class sync_s
{
    GLsync sync_;

  public:
    sync_s();
    ~sync_s();

    sync_s(const sync_s&) = delete;
    sync_s(sync_s&&) noexcept;

    void gpu_wait();
    bool cpu_wait(std::chrono::nanoseconds timeout);
};
} // namespace miximus::gpu
