#include "hardware_devices.hpp"

#include "gpu/device.hpp"
#include "hardware_device.hpp"
#include "logger/logger.hpp"
#include "platform_ingest.hpp"

#include <vector>

namespace miximus::nodes::ffmpeg::detail {

struct hardware_devices_s::impl_s
{
    struct entry_s
    {
        AVHWDeviceType type;
        AVBufferRef*   reference{};
    };

    std::vector<entry_s> devices;

    impl_s()                         = default;
    impl_s(const impl_s&)            = delete;
    impl_s& operator=(const impl_s&) = delete;
    impl_s(impl_s&&)                 = delete;
    impl_s& operator=(impl_s&&)      = delete;

    ~impl_s()
    {
        for (auto& device : devices) {
            av_buffer_unref(&device.reference);
        }
    }
};

hardware_devices_s::hardware_devices_s(gpu::device_s& device)
    : impl_(std::make_unique<impl_s>())
{
    for (const auto type : miximus::ffmpeg::hardware_candidates()) {
        auto& entry = impl_->devices.emplace_back(impl_s::entry_s{.type = type});
        try {
            const auto result = type == AV_HWDEVICE_TYPE_VULKAN
                                    ? create_vulkan_decode_device(&entry.reference, device.diagnostics())
                                    : create_platform_decode_device(&entry.reference, type, &device);
            if (result < 0) {
                av_buffer_unref(&entry.reference);
                getlog("ffmpeg")->info(
                    "Playback device {} unavailable at startup ({})", av_hwdevice_get_type_name(type), result);
            }
        } catch (const std::exception& error) {
            av_buffer_unref(&entry.reference);
            getlog("ffmpeg")->info(
                "Playback device {} unavailable at startup: {}", av_hwdevice_get_type_name(type), error.what());
        }
    }
}

hardware_devices_s::~hardware_devices_s() = default;

AVBufferRef* hardware_devices_s::acquire(AVHWDeviceType type) const
{
    for (const auto& device : impl_->devices) {
        if (device.type == type && device.reference != nullptr) {
            auto* reference = av_buffer_ref(device.reference);
            if (reference == nullptr) {
                throw std::bad_alloc{};
            }
            return reference;
        }
    }
    return nullptr;
}

} // namespace miximus::nodes::ffmpeg::detail
