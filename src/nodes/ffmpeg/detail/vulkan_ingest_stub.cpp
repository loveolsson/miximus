#include "vulkan_ingest.hpp"

namespace miximus::nodes::ffmpeg::detail {

struct vulkan_ingest_s::impl_s
{
};

vulkan_ingest_s::vulkan_ingest_s(gpu::device_s& /* device */) {}

vulkan_ingest_s::~vulkan_ingest_s() = default;

// Keep the member API identical to the supported platform implementation.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void vulkan_ingest_s::convert(decoded_frame_s& /* frame */, const gpu::texture_s& /* target */)
{
    throw std::runtime_error("Vulkan surface ingress is unavailable on this platform");
}

} // namespace miximus::nodes::ffmpeg::detail
