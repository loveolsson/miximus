#include "texture_frame.hpp"

#include "context.hpp"
#include "sync.hpp"

#include <chrono>
#include <utility>

namespace miximus::gpu {

texture_frame_s::texture_frame_s(vec2i_t dimensions, texture_s::format_e format)
    : texture_(std::make_unique<texture_s>(dimensions, format))
{
}

texture_frame_s::~texture_frame_s() = default;

void texture_frame_s::publish_ready_from_worker()
{
    const std::scoped_lock lock(sync_mutex_);
    ready_sync_ = std::make_unique<sync_s>();
    context_s::flush();
}

void texture_frame_s::wait_ready_on_gpu()
{
    std::unique_ptr<sync_s> ready;
    {
        const std::scoped_lock lock(sync_mutex_);
        ready = std::move(ready_sync_);
    }
    if (ready) {
        ready->gpu_wait();
    }
}

void texture_frame_s::release_from_render()
{
    const std::scoped_lock lock(sync_mutex_);
    release_sync_ = std::make_unique<sync_s>();
    context_s::flush();
}

bool texture_frame_s::wait_released_on_worker()
{
    std::unique_ptr<sync_s> released;
    {
        const std::scoped_lock lock(sync_mutex_);
        released = std::move(release_sync_);
    }
    return !released || released->cpu_wait(std::chrono::hours(1));
}

} // namespace miximus::gpu
