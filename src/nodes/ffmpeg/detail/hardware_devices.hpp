#pragma once

#include "wrapper/ffmpeg/hardware.hpp"

#include <memory>

namespace miximus::gpu {
class device_s;
}

namespace miximus::nodes::ffmpeg::detail {

// Initialized before rendering starts, immutable during playback. FFmpeg device
// contexts provide their own queue/context locking for concurrent decoders.
class hardware_devices_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    explicit hardware_devices_s(gpu::device_s& device);
    ~hardware_devices_s();
    hardware_devices_s(const hardware_devices_s&)            = delete;
    hardware_devices_s& operator=(const hardware_devices_s&) = delete;

    // Returns an owned reference, or null when initialization was unavailable.
    AVBufferRef* acquire(AVHWDeviceType type) const;
};

} // namespace miximus::nodes::ffmpeg::detail
