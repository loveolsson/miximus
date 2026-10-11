#include "platform_ingest.hpp"

namespace miximus::nodes::ffmpeg::detail {

int create_platform_decode_device(AVBufferRef** result, AVHWDeviceType type, gpu::device_s* /* device */)
{
    return av_hwdevice_ctx_create(result, type, nullptr, nullptr, 0);
}

struct platform_ingest_s::impl_s
{
};

platform_ingest_s::platform_ingest_s(gpu::device_s& /* device */)
{
    throw std::runtime_error("Native hardware surface sharing is not implemented on this platform");
}

platform_ingest_s::~platform_ingest_s() = default;

// Keep the member API identical to the supported platform implementation.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void platform_ingest_s::convert(decoded_frame_s& /* frame */, const gpu::texture_s& /* target */)
{
    throw std::runtime_error("Native hardware surface sharing is unavailable");
}

const char* platform_ingest_s::transfer_path() { return "native GPU surface sharing"; }

} // namespace miximus::nodes::ffmpeg::detail
