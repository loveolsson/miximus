#include "hardware.hpp"

#include <array>

namespace miximus::ffmpeg {

std::span<const AVHWDeviceType> hardware_candidates() noexcept
{
    static constexpr std::array types{AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
    return types;
}

} // namespace miximus::ffmpeg
