#pragma once

#include "wrapper/ffmpeg/hardware.hpp"

struct AVCodecContext;

namespace miximus::gpu {
struct device_diagnostics_s;
}

namespace miximus::nodes::ffmpeg::detail {

int create_vulkan_decode_device(AVBufferRef** device, const gpu::device_diagnostics_s& diagnostics);
int configure_vulkan_decode_frames(AVCodecContext* codec);

} // namespace miximus::nodes::ffmpeg::detail
