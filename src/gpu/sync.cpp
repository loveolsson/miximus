#include "gpu/sync.hpp"

namespace miximus::gpu {
sync::sync()
{
    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

sync::~sync() { glDeleteSync(sync_); }

void sync::gpu_wait(std::chrono::nanoseconds timeout) { glWaitSync(sync_, 0, timeout.count()); }

bool sync::cpu_wait(std::chrono::nanoseconds timeout)
{
    auto res = glClientWaitSync(sync_, 0, timeout.count());
    return res != GL_TIMEOUT_EXPIRED && res != GL_WAIT_FAILED;
}

} // namespace miximus::gpu
