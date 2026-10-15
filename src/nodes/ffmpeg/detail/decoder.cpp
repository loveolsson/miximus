#include "decoder.hpp"

#include "gpu/drawing.hpp"
#include "hardware_device.hpp"
#include "hardware_devices.hpp"
#include "platform_ingest.hpp"
#include "wrapper/ffmpeg/hardware.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/csp.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace miximus::nodes::ffmpeg::detail {

namespace {

std::string error_message(int result, std::string_view operation)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(result, text.data(), text.size());
    return std::format("{}: {} (FFmpeg {})", operation, text.data(), result);
}

void check_source(int result, std::string_view operation)
{
    if (result < 0) {
        throw source_error_s(result, operation);
    }
}

void check(int result, std::string_view operation)
{
    if (result >= 0) {
        return;
    }

    throw std::runtime_error(error_message(result, operation));
}

utils::flicks timestamp(int64_t value, AVRational time_base)
{
    return utils::flicks{av_rescale_q(value, time_base, {.num = 1, .den = 705600000})};
}

struct frame_deleter_s
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

using frame_ptr_t = std::unique_ptr<AVFrame, frame_deleter_s>;

frame_ptr_t allocate_frame()
{
    auto result = frame_ptr_t{av_frame_alloc()};
    if (!result) {
        throw std::bad_alloc{};
    }

    return result;
}

const AVCodec* find_hardware_decoder(AVCodecID codec_id, AVHWDeviceType type)
{
    void*          iterator = nullptr;
    const AVCodec* decoder  = nullptr;
    while (const auto* candidate = av_codec_iterate(&iterator)) {
        if (av_codec_is_decoder(candidate) == 0 || candidate->id != codec_id) {
            continue;
        }

        for (int j = 0;; ++j) {
            const auto* config = avcodec_get_hw_config(candidate, j);
            if (config == nullptr) {
                break;
            }

            if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                decoder = candidate;
                break;
            }
        }

        if (decoder != nullptr) {
            break;
        }
    }
    return decoder;
}

gpu::color_transform_s frame_color(const AVFrame& frame, const AVPixFmtDescriptor& description)
{
    const bool rgb  = (description.flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool gray = description.nb_components < 3;
    const bool full = frame.color_range == AVCOL_RANGE_JPEG || (rgb && frame.color_range == AVCOL_RANGE_UNSPECIFIED);
    gpu::color_transfer_e matrix{};
    switch (frame.colorspace) {
        case AVCOL_SPC_BT2020_NCL:
            matrix = gpu::color_transfer_e::Rec2020;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            matrix = gpu::color_transfer_e::Rec601;
            break;
        case AVCOL_SPC_UNSPECIFIED:
            matrix = frame.height <= 576 ? gpu::color_transfer_e::Rec601 : gpu::color_transfer_e::Rec709;
            break;
        case AVCOL_SPC_RGB:
        case AVCOL_SPC_BT709:
            matrix = gpu::color_transfer_e::Rec709;
            break;
        default:
            throw std::runtime_error("Unsupported video color matrix");
    }

    gpu::video_transfer_e transfer{};
    switch (frame.color_trc) {
        case AVCOL_TRC_UNSPECIFIED:
            transfer = rgb ? gpu::video_transfer_e::srgb : gpu::video_transfer_e::rec709;
            break;
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:
            transfer = gpu::video_transfer_e::rec709;
            break;
        case AVCOL_TRC_IEC61966_2_1:
            transfer = gpu::video_transfer_e::srgb;
            break;
        case AVCOL_TRC_LINEAR:
            transfer = gpu::video_transfer_e::linear;
            break;
        case AVCOL_TRC_GAMMA22:
            transfer = gpu::video_transfer_e::gamma22;
            break;
        case AVCOL_TRC_GAMMA28:
            transfer = gpu::video_transfer_e::gamma28;
            break;
        default:
            throw std::runtime_error(
                "Unsupported video transfer function (HDR needs a working-space tone-mapping policy)");
    }

    const auto  primary_id = frame.color_primaries == AVCOL_PRI_UNSPECIFIED ? AVCOL_PRI_BT709 : frame.color_primaries;
    const auto* primaries  = av_csp_primaries_desc_from_id(primary_id);
    if (primaries == nullptr) {
        throw std::runtime_error("Unsupported video primaries");
    }

    auto                   xy    = [](const AVCIExy& value) { return gpu::vec2_t{av_q2d(value.x), av_q2d(value.y)}; };
    const auto             gamut = gpu::video_gamut_to_rec709({.red   = xy(primaries->prim.r),
                                                               .green = xy(primaries->prim.g),
                                                               .blue  = xy(primaries->prim.b),
                                                               .white = xy(primaries->wp)});
    gpu::color_transform_s result;
    if (!rgb && !gray) {
        result = gpu::color_parameters(gpu::get_color_transfer_from_yuv(matrix, description.comp[0].depth, full),
                                       gamut,
                                       gpu::color_conversion_direction_e::from_yuv);
    } else {
        gpu::color_conversion_s conversion{.matrix = gpu::mat3{1.0F}, .offset = gpu::vec3_t{0.0F}};
        if (!full) {
            const unsigned depth = description.comp[0].depth;
            if (depth < 8) {
                throw std::runtime_error("Unsupported limited-range RGB depth");
            }

            const auto maximum = static_cast<float>((1U << depth) - 1U);
            const auto scale   = static_cast<float>(1U << (depth - 8));
            conversion.matrix  = gpu::mat3{maximum / (219.0F * scale)};
            conversion.offset  = gpu::vec3_t{16.0F * scale / maximum};
        }

        result = gpu::color_parameters(conversion, gamut, gpu::color_conversion_direction_e::from_yuv);
    }

    result.offset[3] = static_cast<float>(transfer);
    return result;
}

} // namespace

struct decoded_frame_s::impl_s
{
    struct plane_s
    {
        size_t offset{};
        int    row_bytes{};
        int    height{};
    };

    frame_ptr_t                frame{allocate_frame()};
    gpu::video_buffer_header_s header;
    std::array<plane_s, 4>     planes{};

    size_t describe_planes(AVPixelFormat format, const AVPixFmtDescriptor& description, bool host, int count)
    {
        const auto& source = *frame;
        const bool  rgb    = (description.flags & AV_PIX_FMT_FLAG_RGB) != 0;
        size_t      bytes  = sizeof(header);
        for (int plane = 0; plane < count; ++plane) {
            auto& layout      = planes.at(plane);
            layout.row_bytes  = av_image_get_linesize(format, source.width, plane);
            const bool chroma = !rgb && header.component_count >= 3 && (plane == 1 || plane == 2);
            layout.height     = chroma ? AV_CEIL_RSHIFT(source.height, description.log2_chroma_h) : source.height;
            if (layout.row_bytes <= 0 ||
                (host && (std::span(source.data)[plane] == nullptr ||
                          std::abs(int64_t{std::span(source.linesize)[plane]}) < layout.row_bytes))) {
                throw std::runtime_error("Invalid decoded plane stride");
            }

            layout.offset = bytes;
            bytes += static_cast<size_t>(layout.row_bytes) * layout.height;
            bytes = (bytes + 3) & ~size_t{3};
            if (bytes > size_t{256} * 1024 * 1024) {
                throw std::runtime_error("Decoded frame exceeds 256 MiB limit");
            }
        }
        return bytes;
    }
};

decoded_frame_s::decoded_frame_s()
    : impl_(std::make_unique<impl_s>())
{
}

decoded_frame_s::~decoded_frame_s()                                                 = default;
decoded_frame_s::decoded_frame_s(decoded_frame_s&& /* other */) noexcept            = default;
decoded_frame_s& decoded_frame_s::operator=(decoded_frame_s&& /* other */) noexcept = default;

void decoded_frame_s::prepare_host()
{
    if (impl_->frame->hw_frames_ctx != nullptr) {
        throw std::runtime_error("Hardware frames require GPU ingestion; downloading is disabled");
    }

    prepare_layout(true);
}

size_t decoded_frame_s::gpu_storage_bytes() const
{
    if (impl_->frame->format != AV_PIX_FMT_D3D11) {
        return buffer_bytes;
    }

    const auto* frames = reinterpret_cast<AVHWFramesContext*>(impl_->frame->hw_frames_ctx->data);
    // Non-decoder plane texture plus shared carrier plus Vulkan component buffer.
    // Four bytes per coded pixel covers NV12/P010/P016 and carrier row padding.
    return (size_t{static_cast<unsigned>(frames->width)} * static_cast<unsigned>(frames->height) * 4) +
           ((buffer_bytes + 4096) * 2);
}

bool decoded_frame_s::hardware_frame() const { return impl_->frame->hw_frames_ctx != nullptr; }

bool decoded_frame_s::vulkan_frame() const { return impl_->frame->format == AV_PIX_FMT_VULKAN; }

AVFrame* decoded_frame_s::native_frame() const { return impl_->frame.get(); }

const gpu::video_buffer_header_s& decoded_frame_s::component_header() const { return impl_->header; }

void decoded_frame_s::prepare_gpu() { prepare_layout(false); }

void decoded_frame_s::prepare_layout(bool host)
{
    auto& frame = *impl_->frame;
    if (host) {
        check(av_frame_apply_cropping(&frame, 0), "Apply video crop");
    } else if (!hardware_frame() || frame.crop_left != 0 || frame.crop_right != 0 || frame.crop_top != 0 ||
               frame.crop_bottom != 0) {
        throw std::runtime_error("Hardware frame cropping is unsupported by this GPU ingress");
    }

    if (frame.width <= 0 || frame.height <= 0 || frame.width > 16384 || frame.height > 16384) {
        throw std::runtime_error("Invalid decoded frame dimensions");
    }

    if ((frame.flags & AV_FRAME_FLAG_INTERLACED) != 0) {
        throw std::runtime_error("Interlaced playback requires deinterlacing");
    }

    const auto  format      = host ? static_cast<AVPixelFormat>(frame.format)
                                   : reinterpret_cast<AVHWFramesContext*>(frame.hw_frames_ctx->data)->sw_format;
    const auto* description = av_pix_fmt_desc_get(format);
    if (description == nullptr || (description->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL |
                                                         AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_HWACCEL)) != 0) {
        throw std::runtime_error("Unsupported decoded pixel layout");
    }

    dimensions                         = {frame.width, frame.height};
    color                              = frame_color(frame, *description);
    auto&      header                  = impl_->header;
    const bool rgb                     = (description->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    header.encoded_premultiplied_alpha = frame.alpha_mode == AVALPHA_MODE_PREMULTIPLIED ? 1U : 0U;
    header.component_count             = description->nb_components;
    int location_x                     = 0;
    int location_y                     = 128;
    if (frame.chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
        check(av_chroma_location_enum_to_pos(&location_x, &location_y, frame.chroma_location), "Chroma location");
    }

    header.chroma_offset_x = static_cast<float>(location_x) / 256.0F;
    header.chroma_offset_y = static_cast<float>(location_y) / 256.0F;
    const int count        = av_pix_fmt_count_planes(format);
    if (count < 1 || count > 4) {
        throw std::runtime_error("Unsupported decoded plane count");
    }

    buffer_bytes = impl_->describe_planes(format, *description, host, count);

    for (unsigned index = 0; index < header.component_count; ++index) {
        const auto& component = std::span(description->comp)[index];
        const bool  chroma    = !rgb && header.component_count >= 3 && (index == 1 || index == 2);
        const auto& plane     = impl_->planes.at(component.plane);
        if (component.offset < 0 || component.step <= 0) {
            throw std::runtime_error("Unsupported component addressing");
        }

        header.components.at(index) = {
            .offset   = static_cast<uint32_t>(plane.offset + component.offset),
            .stride   = static_cast<uint32_t>(plane.row_bytes),
            .step     = static_cast<uint32_t>(component.step),
            .encoding = gpu::video_component_encoding(component.depth,
                                                      component.shift,
                                                      chroma ? description->log2_chroma_w : 0,
                                                      chroma ? description->log2_chroma_h : 0,
                                                      (description->flags & AV_PIX_FMT_FLAG_BE) != 0),
        };
    }
}

void decoded_frame_s::copy_to(std::span<std::byte> destination) const
{
    if (destination.size() < buffer_bytes) {
        throw std::invalid_argument("Video upload buffer too small");
    }

    std::ranges::fill(destination, std::byte{});
    std::memcpy(destination.data(), &impl_->header, sizeof(impl_->header));
    for (size_t i = 0; i < impl_->planes.size(); ++i) {
        const auto& plane = impl_->planes.at(i);
        for (int y = 0; y < plane.height; ++y) {
            std::memcpy(destination.data() + plane.offset + (static_cast<size_t>(y) * plane.row_bytes),
                        std::span(impl_->frame->data)[i] +
                            (static_cast<ptrdiff_t>(y) * std::span(impl_->frame->linesize)[i]),
                        plane.row_bytes);
        }
    }
}

struct decoder_s::impl_s
{
    gpu::device_s*            gpu_device{};
    AVFormatContext*          format{};
    AVCodecContext*           codec{};
    AVPacket*                 packet{av_packet_alloc()};
    const std::atomic_bool&   stopped;
    const hardware_devices_s* devices{};
    int                       stream{};
    AVPixelFormat             hardware_format{AV_PIX_FMT_NONE};
    std::string               backend_name{"software"};
    std::string               fallback;
    bool                      pending{};
    bool                      draining{};
    std::optional<int64_t>    origin;
    utils::flicks             next_pts{};
    utils::flicks             default_duration{utils::to_flicks(1.0 / 25.0)};

    explicit impl_s(const std::atomic_bool& stop)
        : stopped(stop)
    {
        if (packet == nullptr) {
            throw std::bad_alloc{};
        }
    }

    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;

    ~impl_s()
    {
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    static int interrupt(void* data) { return static_cast<impl_s*>(data)->stopped.load() ? 1 : 0; }

    static AVPixelFormat choose_format(AVCodecContext* context, const AVPixelFormat* formats)
    {
        auto& self = *static_cast<impl_s*>(context->opaque);
        for (auto* entry = formats; *entry != AV_PIX_FMT_NONE; ++entry) {
            if (*entry == self.hardware_format) {
                if (*entry == AV_PIX_FMT_VULKAN) {
                    const int configured = configure_vulkan_decode_frames(context);
                    if (configured < 0) {
                        self.fallback = configured == AVERROR(ENOSYS)
                                            ? "Vulkan video profile does not support FFmpeg's required image usages"
                                            : "Vulkan decode frame-pool initialization failed";
                        return AV_PIX_FMT_NONE;
                    }
                }

                return *entry;
            }
        }

        return AV_PIX_FMT_NONE;
    }

    void feed_packet()
    {
        if (draining) {
            throw std::runtime_error("Decoder requested packets after EOF drain");
        }

        if (!pending) {
            int read{};
            while (true) {
                av_packet_unref(packet);
                read = av_read_frame(format, packet);
                if (read < 0 || packet->stream_index == stream || stopped) {
                    break;
                }
            }

            if (stopped) {
                return;
            }

            if (read < 0 && format->pb != nullptr && format->pb->error < 0) {
                check_source(format->pb->error, "Read video file");
            }
            if (read == AVERROR_EOF) {
                check(avcodec_send_packet(codec, nullptr), "Drain video decoder");
                draining = true;
                return;
            }

            check_source(read, "Read video packet");
            pending = true;
        }

        const int sent = avcodec_send_packet(codec, packet);
        if (sent == AVERROR(EAGAIN)) {
            return;
        }

        if (sent < 0 && backend_name == "vulkan" && !fallback.empty()) {
            throw std::runtime_error(fallback);
        }

        check(sent, "Submit video packet");
        av_packet_unref(packet);
        pending = false;
    }

    void describe_frame(decoded_frame_s& result, AVFrame& frame)
    {
        // Some decoders (including ProRes) return unspecified frame tags
        // even when the container supplied explicit color/alpha metadata.
        // Explicit per-frame values win; only fill the missing fields.
        const auto& parameters = *format->streams[stream]->codecpar;
        if (frame.color_range == AVCOL_RANGE_UNSPECIFIED) {
            frame.color_range = parameters.color_range;
        }

        if (frame.colorspace == AVCOL_SPC_UNSPECIFIED) {
            frame.colorspace = parameters.color_space;
        }

        if (frame.color_trc == AVCOL_TRC_UNSPECIFIED) {
            frame.color_trc = parameters.color_trc;
        }

        if (frame.color_primaries == AVCOL_PRI_UNSPECIFIED) {
            frame.color_primaries = parameters.color_primaries;
        }

        if (frame.chroma_location == AVCHROMA_LOC_UNSPECIFIED) {
            frame.chroma_location = parameters.chroma_location;
        }

        if (frame.alpha_mode == AVALPHA_MODE_UNSPECIFIED) {
            frame.alpha_mode = parameters.alpha_mode;
        }

        const auto time_base = format->streams[stream]->time_base;
        if (frame.best_effort_timestamp != AV_NOPTS_VALUE) {
            if (!origin) {
                origin = frame.best_effort_timestamp;
            }

            result.pts = timestamp(frame.best_effort_timestamp - *origin, time_base);
        } else {
            result.pts = next_pts;
        }

        result.duration = frame.duration > 0 ? timestamp(frame.duration, time_base) : default_duration;
        if (result.duration <= utils::flicks{}) {
            result.duration = default_duration;
        }

        next_pts = result.pts + result.duration;
    }

    void allocate(const AVCodec* decoder)
    {
        avcodec_free_context(&codec);
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr) {
            throw std::bad_alloc{};
        }

        check(avcodec_parameters_to_context(codec, format->streams[stream]->codecpar), "Configure video decoder");
        codec->pkt_timebase = format->streams[stream]->time_base;
        codec->thread_count = 2; // Bound per-session codec concurrency.
    }

    bool open_hardware(const AVCodec* decoder, AVHWDeviceType type, int extra_frames)
    {
        for (int i = 0;; ++i) {
            const auto* config = avcodec_get_hw_config(decoder, i);
            if (config == nullptr) {
                break;
            }

            if (config->device_type != type || (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) {
                continue;
            }

            allocate(decoder);
            int created = 0;
            if (devices != nullptr) {
                codec->hw_device_ctx = devices->acquire(type);
                created              = codec->hw_device_ctx != nullptr ? 0 : AVERROR(ENODEV);
            } else {
                created = type == AV_HWDEVICE_TYPE_VULKAN
                              ? create_vulkan_decode_device(&codec->hw_device_ctx,
                                                            gpu_device != nullptr ? gpu_device->diagnostics()
                                                                                  : gpu::device_diagnostics_s{})
                              : create_platform_decode_device(&codec->hw_device_ctx, type, gpu_device);
            }
            if (created < 0) {
                continue;
            }

            hardware_format        = config->pix_fmt;
            codec->get_format      = choose_format;
            codec->opaque          = this;
            codec->extra_hw_frames = extra_frames;
            if (avcodec_open2(codec, decoder, nullptr) >= 0) {
                backend_name = av_hwdevice_get_type_name(type);
                return true;
            }
        }
        return false;
    }

    bool try_hardware(const AVCodec* software_decoder, int extra_frames, std::span<const std::string> rejected_backends)
    {
        for (auto type : miximus::ffmpeg::hardware_candidates()) {
            // Vulkan Video uses dedicated codec hardware. FFmpeg also offers
            // compute-shader decoders for other codecs; their JIT/startup cost
            // is unsuitable for this playback node. Keep those on native
            // platform hardware or the preferred software decoder.
            if (type == AV_HWDEVICE_TYPE_VULKAN && software_decoder->id != AV_CODEC_ID_H264 &&
                software_decoder->id != AV_CODEC_ID_HEVC && software_decoder->id != AV_CODEC_ID_AV1 &&
                software_decoder->id != AV_CODEC_ID_VP9) {
                continue;
            }

            if (std::ranges::find(rejected_backends, av_hwdevice_get_type_name(type)) != rejected_backends.end()) {
                continue;
            }

            const auto* decoder = find_hardware_decoder(software_decoder->id, type);

            if (decoder == nullptr) {
                continue;
            }

            try {
                if (open_hardware(decoder, type, extra_frames)) {
                    return true;
                }
            } catch (const std::exception& error) {
                fallback = error.what();
            }
        }
        return false;
    }

    void
    open(const std::string& filename, bool hardware, int extra_frames, std::span<const std::string> rejected_backends)
    {
        format = avformat_alloc_context();
        if (format == nullptr) {
            throw std::bad_alloc{};
        }

        format->interrupt_callback = {.callback = interrupt, .opaque = this};
        AVDictionary* options      = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        const int opened = avformat_open_input(&format, filename.c_str(), nullptr, &options);
        av_dict_free(&options);
        check_source(opened, "Open video file");
        check_source(avformat_find_stream_info(format, nullptr), "Read video streams");
        const AVCodec* decoder = nullptr;
        stream                 = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        check(stream, "Find video stream");
        const auto* selected_stream = format->streams[stream];
        const auto* alpha_tag       = av_dict_get(selected_stream->metadata, "alpha_mode", nullptr, 0);
        const auto* pixel_description =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(selected_stream->codecpar->format));
        const bool has_alpha =
            (alpha_tag != nullptr && std::string_view(alpha_tag->value) == "1") ||
            selected_stream->codecpar->alpha_mode != AVALPHA_MODE_UNSPECIFIED ||
            (pixel_description != nullptr && (pixel_description->flags & AV_PIX_FMT_FLAG_ALPHA) != 0);
        if (has_alpha) {
            if (decoder->id == AV_CODEC_ID_VP8 || decoder->id == AV_CODEC_ID_VP9) {
                decoder = avcodec_find_decoder_by_name(decoder->id == AV_CODEC_ID_VP8 ? "libvpx" : "libvpx-vp9");
                if (decoder == nullptr) {
                    throw std::runtime_error("Transparent WebM playback requires FFmpeg's libvpx decoder");
                }
            }

            // Native hardware configurations generally expose opaque NV12/P010.
            // Preserve the alpha plane instead of silently accepting that loss.
            if (hardware) {
                fallback = "Software decoding preserves the video's alpha channel";
            }

            hardware = false;
        }

        const auto rate = av_guess_frame_rate(format, format->streams[stream], nullptr);
        if (rate.num > 0 && rate.den > 0) {
            default_duration = timestamp(1, av_inv_q(rate));
        }

        const auto* software_decoder = decoder;

        if (hardware) {
            if (try_hardware(software_decoder, extra_frames, rejected_backends)) {
                return;
            }
            fallback = "No usable hardware configuration for this codec/device";
        }

        decoder = software_decoder;
        allocate(decoder);
        hardware_format = AV_PIX_FMT_NONE;
        check(avcodec_open2(codec, decoder, nullptr), "Open software decoder");
    }
};

decoder_s::decoder_s(const std::string&           filename,
                     bool                         hardware,
                     int                          extra_frames,
                     const std::atomic_bool&      stopped,
                     std::span<const std::string> rejected_backends,
                     gpu::device_s*               device,
                     const hardware_devices_s*    devices)
    : impl_(std::make_unique<impl_s>(stopped))
{
    impl_->gpu_device = device;
    impl_->devices    = devices;
    impl_->open(filename, hardware, extra_frames, rejected_backends);
}

decoder_s::~decoder_s() = default;

std::string decoder_s::backend() const { return impl_->backend_name; }

std::string decoder_s::fallback_reason() const { return impl_->fallback; }

std::unique_ptr<decoded_frame_s> decoder_s::next()
{
    auto& s      = *impl_;
    auto  result = std::make_unique<decoded_frame_s>();
    while (!s.stopped) {
        const int received = avcodec_receive_frame(s.codec, result->impl_->frame.get());
        if (received == AVERROR_EOF) {
            return nullptr;
        }

        if (received >= 0) {
            auto& frame = *result->impl_->frame;
            if (s.hardware_format != AV_PIX_FMT_NONE &&
                (frame.format != s.hardware_format || frame.hw_frames_ctx == nullptr)) {
                throw std::runtime_error("Hardware decoder did not return a GPU surface; retrying another decoder");
            }

            s.describe_frame(*result, frame);
            return result;
        }

        if (received != AVERROR(EAGAIN)) {
            if (s.backend_name == "vulkan" && !s.fallback.empty()) {
                throw std::runtime_error(s.fallback);
            }

            check(received, "Decode video frame");
        }

        s.feed_packet();
    }

    return nullptr;
}

source_error_s::source_error_s(int code, std::string_view operation)
    : std::runtime_error(error_message(code, operation))
    , code_(code)
{
}

utils::flicks decoder_s::seek(utils::flicks position)
{
    auto&       s        = *impl_;
    const auto* stream   = s.format->streams[s.stream];
    const auto  start    = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    const auto  duration = stream->duration != AV_NOPTS_VALUE ? timestamp(stream->duration, stream->time_base)
                                                              : timestamp(s.format->duration, AV_TIME_BASE_Q);
    position             = std::max(position, utils::flicks{});
    if (duration > utils::flicks{}) {
        position %= duration;
    }
    const auto target = start + av_rescale_q(position.count(), AVRational{1, 705600000}, stream->time_base);
    check_source(avformat_seek_file(s.format, s.stream, std::numeric_limits<int64_t>::min(), target, target, 0),
                 "Seek video file");
    avcodec_flush_buffers(s.codec);
    av_packet_unref(s.packet);
    s.pending = s.draining = false;
    s.origin               = start;
    s.next_pts             = position;
    return position;
}

void decoder_s::rewind()
{
    auto&      s      = *impl_;
    const auto start  = s.format->streams[s.stream]->start_time;
    const auto target = start == AV_NOPTS_VALUE ? 0 : start;
    check_source(avformat_seek_file(s.format, s.stream, std::numeric_limits<int64_t>::min(), target, target, 0),
                 "Rewind video");
    avcodec_flush_buffers(s.codec);
    av_packet_unref(s.packet);
    s.pending = s.draining = false;
    s.origin.reset();
    s.next_pts = {};
}

} // namespace miximus::nodes::ffmpeg::detail
