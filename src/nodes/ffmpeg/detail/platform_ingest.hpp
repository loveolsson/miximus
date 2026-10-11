#pragma once

#include "decoder.hpp"
#include "gpu/device.hpp"
#include "wrapper/ffmpeg/hardware.hpp"

namespace miximus::nodes::ffmpeg::detail {

int create_platform_decode_device(AVBufferRef** result, AVHWDeviceType type, gpu::device_s* device);

class platform_ingest_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    explicit platform_ingest_s(gpu::device_s& device);
    ~platform_ingest_s();
    platform_ingest_s(const platform_ingest_s&)            = delete;
    platform_ingest_s& operator=(const platform_ingest_s&) = delete;
    platform_ingest_s(platform_ingest_s&&)                 = delete;
    platform_ingest_s& operator=(platform_ingest_s&&)      = delete;

    void               convert(decoded_frame_s& frame, const gpu::texture_s& target);
    static const char* transfer_path();
};

} // namespace miximus::nodes::ffmpeg::detail
