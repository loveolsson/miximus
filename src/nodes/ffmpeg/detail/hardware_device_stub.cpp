#include "hardware_device.hpp"

#include <cerrno>

namespace miximus::nodes::ffmpeg::detail {

int create_vulkan_decode_device(AVBufferRef** /* device */, const gpu::device_diagnostics_s& /* diagnostics */)
{
    return AVERROR(ENOSYS);
}

int configure_vulkan_decode_frames(AVCodecContext* /* codec */) { return AVERROR(ENOSYS); }

} // namespace miximus::nodes::ffmpeg::detail
