#include "hardware.hpp"

namespace miximus::ffmpeg {

std::span<const AVHWDeviceType> hardware_candidates() noexcept { return {}; }

} // namespace miximus::ffmpeg
