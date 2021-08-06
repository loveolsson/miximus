#include "gpu/sync.hpp"

namespace miximus::gpu {
sync_s::sync_s()
{
    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

sync_s::~sync_s() { glDeleteSync(sync_); }

void sync_s::gpu_wait() { glWaitSync(sync_, 0, GL_TIMEOUT_IGNORED); }

bool sync_s::cpu_wait(std::chrono::nanoseconds timeout)
{
    auto res = glClientWaitSync(sync_, 0, timeout.count());
    return res != GL_TIMEOUT_EXPIRED;
}

} // namespace miximus::gpu
