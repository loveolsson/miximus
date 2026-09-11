#include "output_path.hpp"

#include "gpu/drawing.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "logger/logger.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <cstddef>
#include <cstdint>

namespace miximus::nodes::decklink::detail {

using namespace decklink_sdk;

namespace {

auto log() { return getlog("decklink"); }

auto make_host_layout(IDeckLinkOutput*                   device,
                      const output_display_mode_s&       display_mode,
                      BMDPixelFormat                     decklink_pixel_format,
                      gpu::transfer::host_pixel_format_e host_pixel_format,
                      std::string_view device_name) -> std::optional<gpu::transfer::host_frame_layout_s>
{
    int32_t    row_bytes{};
    const auto result = device->RowBytesForPixelFormat(decklink_pixel_format, display_mode.dimensions.x, &row_bytes);
    if (result != S_OK || row_bytes <= 0 || row_bytes % 4 != 0) {
        log()->error("Unable to determine DeckLink row bytes for {} with result {:#010x}",
                     device_name,
                     static_cast<uint32_t>(result));
        return std::nullopt;
    }
    return gpu::transfer::host_frame_layout_s{
        .image_dimensions  = display_mode.dimensions,
        .pixel_format      = host_pixel_format,
        .row_stride_bytes  = static_cast<size_t>(row_bytes),
        .buffer_size_bytes = static_cast<size_t>(row_bytes) * display_mode.dimensions.y,
        // The SDK may request write access even for scheduled output buffers.
        // Keep that requirement explicit for registered/pinned-memory backends.
        .memory_access = gpu::transfer::host_memory_access_e::read_write,
    };
}

auto create_frame_with_buffer(IDeckLinkOutput*                          device,
                              IDeckLinkVideoBuffer*                     buffer,
                              const output_display_mode_s&              display_mode,
                              const gpu::transfer::host_frame_layout_s& host_layout,
                              BMDPixelFormat                            decklink_pixel_format,
                              std::string_view device_name) -> decklink_ptr<IDeckLinkMutableVideoFrame>
{
    if (buffer == nullptr) {
        return {};
    }

    decklink_ptr<IDeckLinkMutableVideoFrame> frame;
    const auto                               result = device->CreateVideoFrameWithBuffer(display_mode.dimensions.x,
                                                           display_mode.dimensions.y,
                                                           static_cast<int32_t>(host_layout.row_stride_bytes),
                                                           decklink_pixel_format,
                                                           bmdFrameFlagDefault,
                                                           buffer,
                                                           frame.releaseAndGetAddressOf());
    if (result != S_OK) {
        log()->error("CreateVideoFrameWithBuffer failed for {} with result {:#010x}",
                     device_name,
                     static_cast<uint32_t>(result));
        return {};
    }
    return frame;
}

class output_frame_renderer_s final : public output_frame_renderer_i
{
    gpu::color_transform_s             color_;
    size_t                             stride_;
    gpu::transfer::host_pixel_format_e pixel_format_;

  public:
    output_frame_renderer_s(const output_display_mode_s& mode, const gpu::transfer::host_frame_layout_s& layout)
        : color_(gpu::color_parameters(mode.yuv_conversion,
                                       mode.gamut_conversion,
                                       gpu::color_conversion_direction_e::to_yuv))
        , stride_(layout.row_stride_bytes)
        , pixel_format_(layout.pixel_format)
    {
    }

    void render(gpu::recording_s&                         commands,
                const gpu::texture_s*                     source,
                gpu::transfer::texture_readback_target_s& target,
                gpu::fill_mode_e                          fill_mode) final
    {
        auto& scaled = *target.conversion_texture();
        scaled.clear(commands);
        if (source != nullptr) {
            const auto geometry = gpu::calculate_texture_draw({}, source->dimensions(), scaled.dimensions(), fill_mode);
            gpu::draw_texture(commands, source, &scaled, geometry);
        }

        if (pixel_format_ == gpu::transfer::host_pixel_format_e::argb_u8) {
            gpu::draw_texture(commands,
                              &scaled,
                              target.texture(),
                              {},
                              1,
                              gpu::color_operation_e::encode_rec709_premultiplied,
                              gpu::compositing_e::replace,
                              target.output_order());
        } else {
            commands.pack_v210(scaled, target.buffer(), color_, stride_);
        }
    }
};

} // namespace

output_path_i::output_path_i(output_display_mode_s              display_mode,
                             BMDPixelFormat                     decklink_pixel_format,
                             gpu::transfer::host_frame_layout_s host_layout)
    : display_mode_(display_mode)
    , decklink_pixel_format_(decklink_pixel_format)
    , host_layout_(host_layout)
{
}

v210_output_path_s::v210_output_path_s(output_display_mode_s              display_mode,
                                       gpu::transfer::host_frame_layout_s host_layout)
    : output_path_i(display_mode, bmdFormat10BitYUV, host_layout)
{
}

auto v210_output_path_s::create(IDeckLinkOutput*             device,
                                const output_display_mode_s& display_mode,
                                std::string_view             device_name) -> std::shared_ptr<const output_path_i>
{
    auto host_layout = make_host_layout(
        device, display_mode, bmdFormat10BitYUV, gpu::transfer::host_pixel_format_e::v210, device_name);
    if (!host_layout) {
        return {};
    }
    return std::shared_ptr<const output_path_i>(new v210_output_path_s(display_mode, *host_layout));
}

auto v210_output_path_s::create_frame(IDeckLinkOutput*      device,
                                      IDeckLinkVideoBuffer* buffer,
                                      std::string_view      device_name) const -> decklink_ptr<IDeckLinkVideoFrame>
{
    auto frame =
        create_frame_with_buffer(device, buffer, display_mode(), host_layout(), decklink_pixel_format(), device_name);
    if (!frame) {
        return {};
    }
    auto metadata = frame.query<IDeckLinkVideoFrameMutableMetadataExtensions>();
    if (metadata) {
        (void)metadata->SetInt(bmdDeckLinkFrameMetadataColorspace, display_mode().colorspace);
    }
    return frame.query<IDeckLinkVideoFrame>();
}

auto v210_output_path_s::create_renderer() const -> std::unique_ptr<output_frame_renderer_i>
{
    return std::make_unique<output_frame_renderer_s>(display_mode(), host_layout());
}

premultiplied_argb_output_path_s::premultiplied_argb_output_path_s(output_display_mode_s              display_mode,
                                                                   gpu::transfer::host_frame_layout_s host_layout)
    : output_path_i(display_mode, bmdFormat8BitARGB, host_layout)
{
}

auto premultiplied_argb_output_path_s::create(IDeckLinkOutput*             device,
                                              const output_display_mode_s& display_mode,
                                              std::string_view device_name) -> std::shared_ptr<const output_path_i>
{
    // Preserve alpha in the SDK's explicit ARGB byte order.
    auto host_layout = make_host_layout(
        device, display_mode, bmdFormat8BitARGB, gpu::transfer::host_pixel_format_e::argb_u8, device_name);
    if (!host_layout) {
        return {};
    }

    return std::shared_ptr<const output_path_i>(new premultiplied_argb_output_path_s(display_mode, *host_layout));
}

auto premultiplied_argb_output_path_s::create_frame(IDeckLinkOutput*      device,
                                                    IDeckLinkVideoBuffer* buffer,
                                                    std::string_view      device_name) const
    -> decklink_ptr<IDeckLinkVideoFrame>
{
    return create_frame_with_buffer(device, buffer, display_mode(), host_layout(), decklink_pixel_format(), device_name)
        .query<IDeckLinkVideoFrame>();
}

auto premultiplied_argb_output_path_s::create_renderer() const -> std::unique_ptr<output_frame_renderer_i>
{
    return std::make_unique<output_frame_renderer_s>(display_mode(), host_layout());
}

} // namespace miximus::nodes::decklink::detail
