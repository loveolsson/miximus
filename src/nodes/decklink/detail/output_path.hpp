#pragma once

#include "gpu/color_transfer.hpp"
#include "gpu/geometry.hpp"
#include "gpu/transfer/texture_transfer.hpp"
#include "utils/flicks.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace miximus::gpu {
class context_s;
class texture_s;
namespace transfer {
class texture_readback_target_s;
}
} // namespace miximus::gpu

namespace miximus::nodes::decklink::detail {

enum class keyer_mode_e : uint8_t
{
    disabled,
    internal,
    external,
};

struct output_display_mode_s
{
    BMDDisplayMode          mode{bmdModeUnknown};
    BMDTimeValue            frame_duration{};
    BMDTimeScale            time_scale{};
    gpu::vec2i_t            dimensions{};
    utils::flicks           frame_duration_flicks{};
    gpu::color_conversion_s yuv_conversion{};
    gpu::mat3               gamut_conversion{1.0F};
    BMDColorspace           colorspace{bmdColorspaceRec709};
};

class output_frame_renderer_i
{
  public:
    virtual ~output_frame_renderer_i() = default;

    output_frame_renderer_i()                                          = default;
    output_frame_renderer_i(const output_frame_renderer_i&)            = delete;
    output_frame_renderer_i& operator=(const output_frame_renderer_i&) = delete;
    output_frame_renderer_i(output_frame_renderer_i&&)                 = delete;
    output_frame_renderer_i& operator=(output_frame_renderer_i&&)      = delete;

    virtual void
    render(gpu::texture_s* source, gpu::transfer::texture_readback_target_s& target, gpu::fill_mode_e fill_mode) = 0;
};

class output_path_i
{
    output_display_mode_s                    display_mode_;
    BMDPixelFormat                           decklink_pixel_format_;
    gpu::transfer::texture_transfer_layout_s transfer_layout_;

  protected:
    output_path_i(output_display_mode_s                    display_mode,
                  BMDPixelFormat                           decklink_pixel_format,
                  gpu::transfer::texture_transfer_layout_s transfer_layout);

  public:
    virtual ~output_path_i() = default;

    output_path_i(const output_path_i&)            = delete;
    output_path_i& operator=(const output_path_i&) = delete;
    output_path_i(output_path_i&&)                 = delete;
    output_path_i& operator=(output_path_i&&)      = delete;

    const output_display_mode_s& display_mode() const noexcept { return display_mode_; }
    BMDPixelFormat               decklink_pixel_format() const noexcept { return decklink_pixel_format_; }
    const gpu::transfer::texture_transfer_layout_s& transfer_layout() const noexcept { return transfer_layout_; }

    virtual auto create_frame(IDeckLinkOutput* device, IDeckLinkVideoBuffer* buffer, std::string_view device_name) const
        -> decklink_sdk::decklink_ptr<IDeckLinkVideoFrame>                                                  = 0;
    virtual auto create_renderer(gpu::context_s* context) const -> std::unique_ptr<output_frame_renderer_i> = 0;
};

class v210_output_path_s final : public output_path_i
{
    v210_output_path_s(output_display_mode_s display_mode, gpu::transfer::texture_transfer_layout_s transfer_layout);

  public:
    static auto create(IDeckLinkOutput* device, const output_display_mode_s& display_mode, std::string_view device_name)
        -> std::shared_ptr<const output_path_i>;

    auto create_frame(IDeckLinkOutput* device, IDeckLinkVideoBuffer* buffer, std::string_view device_name) const
        -> decklink_sdk::decklink_ptr<IDeckLinkVideoFrame> final;
    auto create_renderer(gpu::context_s* context) const -> std::unique_ptr<output_frame_renderer_i> final;
};

class premultiplied_bgra_output_path_s final : public output_path_i
{
    premultiplied_bgra_output_path_s(output_display_mode_s                    display_mode,
                                     gpu::transfer::texture_transfer_layout_s transfer_layout);

  public:
    static auto create(IDeckLinkOutput* device, const output_display_mode_s& display_mode, std::string_view device_name)
        -> std::shared_ptr<const output_path_i>;

    auto create_frame(IDeckLinkOutput* device, IDeckLinkVideoBuffer* buffer, std::string_view device_name) const
        -> decklink_sdk::decklink_ptr<IDeckLinkVideoFrame> final;
    auto create_renderer(gpu::context_s* context) const -> std::unique_ptr<output_frame_renderer_i> final;
};

struct active_output_s
{
    std::shared_ptr<const output_path_i> path;
    keyer_mode_e                         requested_keyer_mode{keyer_mode_e::disabled};
    keyer_mode_e                         active_keyer_mode{keyer_mode_e::disabled};
    std::optional<std::string>           keyer_fallback_reason;
};

} // namespace miximus::nodes::decklink::detail
