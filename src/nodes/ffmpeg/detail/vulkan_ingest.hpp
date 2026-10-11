#pragma once

#include "decoder.hpp"
#include "gpu/device.hpp"

namespace miximus::nodes::ffmpeg::detail {

// Worker-owned GPU bridge. It retains FFmpeg's device and copies raw components
// between devices on the same GPU; all color conversion stays in gpu::recording_s.
class vulkan_ingest_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    explicit vulkan_ingest_s(gpu::device_s& device);
    ~vulkan_ingest_s();
    vulkan_ingest_s(const vulkan_ingest_s& other)            = delete;
    vulkan_ingest_s& operator=(const vulkan_ingest_s& other) = delete;
    vulkan_ingest_s(vulkan_ingest_s&& other)                 = delete;
    vulkan_ingest_s& operator=(vulkan_ingest_s&& other)      = delete;

    void convert(decoded_frame_s& frame, const gpu::texture_s& target);
};

} // namespace miximus::nodes::ffmpeg::detail
