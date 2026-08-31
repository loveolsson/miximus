#include "output_path.hpp"

#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/textured_quad.hpp"
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
        .memory_access     = gpu::transfer::host_memory_access_e::read_only,
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

class scaled_output_frame_renderer_s : public output_frame_renderer_i
{
    gpu::vec2i_t                          readback_dimensions_;
    std::unique_ptr<gpu::framebuffer_s>   scaled_framebuffer_;
    std::unique_ptr<gpu::textured_quad_s> scale_quad_;

  protected:
    explicit scaled_output_frame_renderer_s(gpu::context_s*                  context,
                                            const output_display_mode_s&     display_mode,
                                            gpu::vec2i_t                     readback_dimensions,
                                            gpu::texture_s::storage_format_e scaled_storage_format)
        : readback_dimensions_(readback_dimensions)
        , scaled_framebuffer_(std::make_unique<gpu::framebuffer_s>(display_mode.dimensions, scaled_storage_format))
        , scale_quad_(std::make_unique<gpu::textured_quad_s>(context->get_shader(gpu::shader_program_s::name_e::basic)))
    {
    }

    gpu::texture_s* scaled_texture() const noexcept { return scaled_framebuffer_->texture(); }
    virtual void    render_output(gpu::transfer::texture_readback_target_s& target) = 0;

  public:
    void
    render(gpu::texture_s* source, gpu::transfer::texture_readback_target_s& target, gpu::fill_mode_e fill_mode) final
    {
        scaled_framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        const auto texture_draw = gpu::calculate_texture_draw(
            {}, source->display_dimensions(), scaled_framebuffer_->texture()->display_dimensions(), fill_mode);
        scale_quad_->draw(source, texture_draw);
        gpu::framebuffer_s::end_render();

        target.framebuffer()->begin_render(
            {
                .pos  = {0, 0},
                .size = readback_dimensions_,
        },
            gpu::framebuffer_s::load_op_e::clear);
        render_output(target);
        gpu::framebuffer_s::end_render();
    }
};

class v210_output_frame_renderer_s final : public scaled_output_frame_renderer_s
{
    gpu::color_conversion_s               yuv_conversion_;
    gpu::mat3                             gamut_conversion_;
    int                                   target_width_;
    std::unique_ptr<gpu::textured_quad_s> output_quad_;

    void render_output(gpu::transfer::texture_readback_target_s& target) final
    {
        auto* shader = output_quad_->shader();
        shader->set_uniform("target_width", target_width_);
        shader->set_uniform("transfer", yuv_conversion_.matrix);
        shader->set_uniform("transfer_offset", yuv_conversion_.offset);
        shader->set_uniform("gamut_transfer", gamut_conversion_);
        target.draw(output_quad_.get(), scaled_texture());
    }

  public:
    v210_output_frame_renderer_s(gpu::context_s*                           context,
                                 const output_display_mode_s&              display_mode,
                                 const gpu::transfer::host_frame_layout_s& host_layout)
        : scaled_output_frame_renderer_s(
              context,
              display_mode,
              {static_cast<int>(host_layout.row_stride_bytes / 4), host_layout.image_dimensions.y},
              gpu::texture_s::storage_format_e::rgb_unorm16)
        , yuv_conversion_(display_mode.yuv_conversion)
        , gamut_conversion_(display_mode.gamut_conversion)
        , target_width_(static_cast<int>(host_layout.row_stride_bytes / 4))
        , output_quad_(
              std::make_unique<gpu::textured_quad_s>(context->get_shader(gpu::shader_program_s::name_e::rgb_to_yuv)))
    {
    }
};

class premultiplied_bgra_output_frame_renderer_s final : public scaled_output_frame_renderer_s
{
    std::unique_ptr<gpu::textured_quad_s> output_quad_;

    void render_output(gpu::transfer::texture_readback_target_s& target) final
    {
        target.draw(output_quad_.get(), scaled_texture());
    }

  public:
    premultiplied_bgra_output_frame_renderer_s(gpu::context_s*                           context,
                                               const output_display_mode_s&              display_mode,
                                               const gpu::transfer::host_frame_layout_s& host_layout)
        : scaled_output_frame_renderer_s(context,
                                         display_mode,
                                         host_layout.image_dimensions,
                                         gpu::texture_s::storage_format_e::rgba_unorm16)
        , output_quad_(std::make_unique<gpu::textured_quad_s>(
              context->get_shader(gpu::shader_program_s::name_e::encode_rec709_premultiplied)))
    {
        output_quad_->set_blending_enabled(false);
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

auto v210_output_path_s::create_renderer(gpu::context_s* context) const -> std::unique_ptr<output_frame_renderer_i>
{
    return std::make_unique<v210_output_frame_renderer_s>(context, display_mode(), host_layout());
}

premultiplied_bgra_output_path_s::premultiplied_bgra_output_path_s(output_display_mode_s              display_mode,
                                                                   gpu::transfer::host_frame_layout_s host_layout)
    : output_path_i(display_mode, bmdFormat8BitARGB, host_layout)
{
}

auto premultiplied_bgra_output_path_s::create(IDeckLinkOutput*             device,
                                              const output_display_mode_s& display_mode,
                                              std::string_view device_name) -> std::shared_ptr<const output_path_i>
{
    // DeckLink's ARGB descriptor corresponds to the byte layout consumed by
    // this little-endian BGRA readback path.
    auto host_layout = make_host_layout(
        device, display_mode, bmdFormat8BitARGB, gpu::transfer::host_pixel_format_e::argb_u8, device_name);
    if (!host_layout) {
        return {};
    }
    return std::shared_ptr<const output_path_i>(new premultiplied_bgra_output_path_s(display_mode, *host_layout));
}

auto premultiplied_bgra_output_path_s::create_frame(IDeckLinkOutput*      device,
                                                    IDeckLinkVideoBuffer* buffer,
                                                    std::string_view      device_name) const
    -> decklink_ptr<IDeckLinkVideoFrame>
{
    return create_frame_with_buffer(device, buffer, display_mode(), host_layout(), decklink_pixel_format(), device_name)
        .query<IDeckLinkVideoFrame>();
}

auto premultiplied_bgra_output_path_s::create_renderer(gpu::context_s* context) const
    -> std::unique_ptr<output_frame_renderer_i>
{
    return std::make_unique<premultiplied_bgra_output_frame_renderer_s>(context, display_mode(), host_layout());
}

} // namespace miximus::nodes::decklink::detail
