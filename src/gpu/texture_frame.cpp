#include "texture_frame.hpp"

#include "context.hpp"
#include "fence.hpp"

#include <chrono>
#include <utility>

namespace miximus::gpu {

texture_frame_s::texture_frame_s(vec2i_t dimensions, texture_s::pixel_format_e pixel_format)
    : texture_(std::make_unique<texture_s>(dimensions, pixel_format))
{
}

texture_frame_s::~texture_frame_s() = default;

void texture_frame_s::publish_upload_ready_fence()
{
    const std::scoped_lock lock(fence_mutex_);
    upload_ready_fence_ = std::make_unique<fence_s>();
    context_s::flush();
}

void texture_frame_s::wait_for_upload_on_gpu()
{
    std::unique_ptr<fence_s> upload_ready_fence;
    {
        const std::scoped_lock lock(fence_mutex_);
        upload_ready_fence = std::move(upload_ready_fence_);
    }
    if (upload_ready_fence) {
        upload_ready_fence->gpu_wait();
    }
}

void texture_frame_s::publish_render_release_fence()
{
    const std::scoped_lock lock(fence_mutex_);
    render_release_fence_ = std::make_unique<fence_s>();
    context_s::flush();
}

bool texture_frame_s::wait_for_render_release_on_worker()
{
    std::unique_ptr<fence_s> render_release_fence;
    {
        const std::scoped_lock lock(fence_mutex_);
        render_release_fence = std::move(render_release_fence_);
    }
    return !render_release_fence || render_release_fence->cpu_wait(std::chrono::hours(1));
}

} // namespace miximus::gpu
