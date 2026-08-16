#pragma once
#include "gpu/glad.hpp"

#include <chrono>
#include <mutex>

namespace miximus::gpu {
class fence_s
{
    GLsync gl_sync_;

  public:
    fence_s();
    ~fence_s();

    fence_s(const fence_s&) = delete;
    fence_s(fence_s&&) noexcept;

    void gpu_wait();
    bool cpu_wait(std::chrono::nanoseconds timeout);
};
} // namespace miximus::gpu
