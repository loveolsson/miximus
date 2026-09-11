#pragma once

#include "buffer.hpp"
#include "completion.hpp"
#include "texture.hpp"
#include "transfer/texture_transfer.hpp"

#include <memory>

namespace miximus::gpu {

// CPU leases and native recording/timeline uses must both retire before reuse.
class texture_frame_s
{
    texture_s                     texture_;
    buffer_s                      buffer_;
    transfer::host_frame_layout_s layout_;
    completion_s                  upload_completion_;
    std::shared_ptr<texture_s>    conversion_texture_;

  public:
    texture_frame_s(device_s& device, transfer::host_frame_layout_s layout, sampling_e sampling);
    texture_s*                           texture() noexcept { return texture_ ? &texture_ : nullptr; }
    const texture_s*                     texture() const noexcept { return texture_ ? &texture_ : nullptr; }
    const buffer_s&                      buffer() const noexcept { return buffer_; }
    const transfer::host_frame_layout_s& layout() const noexcept { return layout_; }
    const std::shared_ptr<texture_s>&    conversion_texture() const noexcept { return conversion_texture_; }
    void set_conversion_texture(std::shared_ptr<texture_s> texture) { conversion_texture_ = std::move(texture); }
    const completion_s& upload_completion() const noexcept { return upload_completion_; }
    void                set_upload_completion(completion_s completion) { upload_completion_ = std::move(completion); }
    bool                ready_for_reuse() const { return texture_.idle() && buffer_.idle(); }
};

using texture_frame_ptr = std::shared_ptr<texture_frame_s>;
} // namespace miximus::gpu
