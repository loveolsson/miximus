#include "gpu/sync.hpp"

namespace miximus::gpu {
sync_s::sync_s()
{
    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

sync_s::~sync_s()
{
    if (sync_ != nullptr) {
        glDeleteSync(sync_);
    }
}

sync_s::sync_s(sync_s&& o)
    : sync_(o.sync_)
{
    o.sync_ = nullptr;
}

void sync_s::gpu_wait()
{
    if (sync_ != nullptr) {
        glWaitSync(sync_, 0, GL_TIMEOUT_IGNORED);
    }
}

bool sync_s::cpu_wait(std::chrono::nanoseconds timeout)
{
    if (sync_ == nullptr) {
        return false;
    }

    auto res = glClientWaitSync(sync_, GL_SYNC_FLUSH_COMMANDS_BIT, timeout.count());
    return res != GL_TIMEOUT_EXPIRED;
}

} // namespace miximus::gpu
