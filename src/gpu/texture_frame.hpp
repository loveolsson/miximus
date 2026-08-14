#pragma once

#include "texture.hpp"

#include <memory>
#include <mutex>

namespace miximus::gpu {

class sync_s;

// A texture shared by two GL contexts. The producing context publishes a
// ready fence; the consuming render context publishes a release fence after
// its last use. Pooling and transfer policy deliberately live elsewhere.
class texture_frame_s
{
    std::unique_ptr<texture_s> texture_;
    std::mutex                 sync_mutex_;
    std::unique_ptr<sync_s>    ready_sync_;
    std::unique_ptr<sync_s>    release_sync_;

  public:
    texture_frame_s(vec2i_t dimensions, texture_s::format_e format);
    ~texture_frame_s();

    texture_frame_s(const texture_frame_s&)            = delete;
    texture_frame_s(texture_frame_s&&)                 = delete;
    texture_frame_s& operator=(const texture_frame_s&) = delete;
    texture_frame_s& operator=(texture_frame_s&&)      = delete;

    texture_s* texture() const noexcept { return texture_.get(); }

    // Called with the worker context current after all producer-side commands.
    void publish_ready_from_worker();

    // Called with the render context current before sampling the texture.
    void wait_ready_on_gpu();

    // Called with the render context current after the traversal's last use.
    void release_from_render();

    // Called by the worker before beginning a new transaction on the texture.
    bool wait_released_on_worker();
};

using texture_frame_ptr = std::shared_ptr<texture_frame_s>;

} // namespace miximus::gpu
