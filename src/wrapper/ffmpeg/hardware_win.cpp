#include "hardware.hpp"

#include <array>

namespace miximus::ffmpeg {

std::span<const AVHWDeviceType> hardware_candidates() noexcept
{
    static constexpr std::array types{AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_D3D11VA};
    return types;
}

} // namespace miximus::ffmpeg
