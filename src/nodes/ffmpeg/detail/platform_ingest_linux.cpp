#include "platform_ingest.hpp"
#include "vulkan_ingest.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <array>
#include <format>

namespace miximus::nodes::ffmpeg::detail {
namespace {

void check(int result, const char* operation)
{
    if (result < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> error{};
        av_strerror(result, error.data(), error.size());
        throw std::runtime_error(std::format("{}: {}", operation, error.data()));
    }
}

} // namespace

int create_platform_decode_device(AVBufferRef** result, AVHWDeviceType type, gpu::device_s* /* device */)
{
    return av_hwdevice_ctx_create(result, type, nullptr, nullptr, 0);
}

struct platform_ingest_s::impl_s
{
    vulkan_ingest_s bridge;
    AVBufferRef*    source_frames{};
    AVBufferRef*    device{};
    AVBufferRef*    mapped_frames{};

    explicit impl_s(gpu::device_s& gpu)
        : bridge(gpu)
    {
    }
    ~impl_s() { reset(); }
    impl_s(const impl_s&)            = delete;
    impl_s& operator=(const impl_s&) = delete;
    impl_s(impl_s&&)                 = delete;
    impl_s& operator=(impl_s&&)      = delete;

    void reset()
    {
        av_buffer_unref(&mapped_frames);
        av_buffer_unref(&device);
        av_buffer_unref(&source_frames);
    }

    void initialize(AVBufferRef* source)
    {
        if (mapped_frames != nullptr && source_frames->data == source->data) {
            return;
        }

        reset();
        source_frames = av_buffer_ref(source);
        if (source_frames == nullptr) {
            throw std::bad_alloc{};
        }

        auto* context = reinterpret_cast<AVHWFramesContext*>(source->data);
        check(av_hwdevice_ctx_create_derived(&device, AV_HWDEVICE_TYPE_VULKAN, context->device_ref, 0),
              "Derive Vulkan device for VAAPI surface sharing");
        check(av_hwframe_ctx_create_derived(
                  &mapped_frames, AV_PIX_FMT_VULKAN, device, source, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT),
              "Derive Vulkan frame mapping");
    }
};

platform_ingest_s::platform_ingest_s(gpu::device_s& device)
    : impl_(std::make_unique<impl_s>(device))
{
}
platform_ingest_s::~platform_ingest_s() = default;

void platform_ingest_s::convert(decoded_frame_s& frame, const gpu::texture_s& target)
{
    auto* source = frame.native_frame();
    if (source->format != AV_PIX_FMT_VAAPI && source->format != AV_PIX_FMT_DRM_PRIME) {
        throw std::runtime_error("Expected VAAPI or DRM hardware surface");
    }

    impl_->initialize(source->hw_frames_ctx);
    decoded_frame_s mapped;
    auto*           native = mapped.native_frame();
    native->format         = AV_PIX_FMT_VULKAN;
    native->hw_frames_ctx  = av_buffer_ref(impl_->mapped_frames);
    if (native->hw_frames_ctx == nullptr) {
        throw std::bad_alloc{};
    }

    // DIRECT prohibits a hidden transfer through host memory. FFmpeg owns the
    // DMA-BUF export/import and synchronization; the bridge checks GPU identity.
    check(av_hwframe_map(native, source, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT),
          "Map video surface to Vulkan (FFmpeg needs libdrm-enabled DRM PRIME sharing)");
    check(av_frame_copy_props(native, source), "Copy mapped video metadata");
    mapped.prepare_gpu();
    impl_->bridge.convert(mapped, target);
}

const char* platform_ingest_s::transfer_path() { return "VAAPI shared GPU surface to Vulkan"; }

} // namespace miximus::nodes::ffmpeg::detail
