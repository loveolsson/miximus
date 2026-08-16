#pragma once

#include "texture.hpp"

#include <memory>
#include <mutex>

namespace miximus::gpu {

class fence_s;

// A texture shared by two GL contexts. The producing context publishes an
// upload-ready fence; the consuming render context publishes a render-release
// fence after its last use. Pooling and transfer policy deliberately live elsewhere.
class texture_frame_s
{
    std::unique_ptr<texture_s> texture_;
    std::mutex                 fence_mutex_;
    std::unique_ptr<fence_s>   upload_ready_fence_;
    std::unique_ptr<fence_s>   render_release_fence_;

  public:
    texture_frame_s(vec2i_t dimensions, texture_s::pixel_format_e pixel_format);
    ~texture_frame_s();

    texture_frame_s(const texture_frame_s&)            = delete;
    texture_frame_s(texture_frame_s&&)                 = delete;
    texture_frame_s& operator=(const texture_frame_s&) = delete;
    texture_frame_s& operator=(texture_frame_s&&)      = delete;

    texture_s* texture() const noexcept { return texture_.get(); }

    // Called with the worker context current after all producer-side commands.
    void publish_upload_ready_fence();

    // Called with the render context current before sampling the texture.
    void wait_for_upload_on_gpu();

    // Called with the render context current after the traversal's last use.
    void publish_render_release_fence();

    // Called by the worker before beginning a new transaction on the texture.
    bool wait_for_render_release_on_worker();
};

using texture_frame_ptr = std::shared_ptr<texture_frame_s>;

} // namespace miximus::gpu
