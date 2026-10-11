#pragma once

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <span>

namespace miximus::ffmpeg {

// Platform policy only. Device creation and codec capability checks use upstream APIs.
std::span<const AVHWDeviceType> hardware_candidates() noexcept;

} // namespace miximus::ffmpeg
