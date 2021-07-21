#pragma once
#include "gpu/glad.hpp"

#include <chrono>

namespace miximus::gpu {
class sync
{
    GLsync sync_;

  public:
    sync();
    ~sync();

    sync(const sync&) = delete;
    sync(sync&&)      = delete;

    void gpu_wait(std::chrono::nanoseconds timeout);
    bool cpu_wait(std::chrono::nanoseconds timeout);
};
} // namespace miximus::gpu
