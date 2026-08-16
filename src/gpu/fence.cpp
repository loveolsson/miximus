#include "gpu/fence.hpp"

#include "gpu/context.hpp"

#include <chrono>

namespace miximus::gpu {
fence_s::fence_s()
    : gl_sync_(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0))
{
}

fence_s::~fence_s()
{
    if (gl_sync_ != nullptr) {
        if (!context_s::require_current()) {
            return;
        }
        glDeleteSync(gl_sync_);
    }
}

fence_s::fence_s(fence_s&& o) noexcept
    : gl_sync_(o.gl_sync_)
{
    o.gl_sync_ = nullptr;
}

void fence_s::gpu_wait()
{
    if (gl_sync_ != nullptr) {
        glWaitSync(gl_sync_, 0, GL_TIMEOUT_IGNORED);
    }
}

bool fence_s::cpu_wait(std::chrono::nanoseconds timeout)
{
    if (gl_sync_ == nullptr) {
        return false;
    }

    auto res = glClientWaitSync(gl_sync_, GL_SYNC_FLUSH_COMMANDS_BIT, timeout.count());
    return res != GL_TIMEOUT_EXPIRED;
}

} // namespace miximus::gpu
