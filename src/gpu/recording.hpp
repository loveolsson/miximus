#pragma once

#include "buffer.hpp"
#include "completion.hpp"
#include "texture.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace miximus::gpu {

// Rows are explicit vec4-aligned values; no implicit C++/GLSL mat3 transpose.
struct color_transform_s
{
    std::array<float, 12> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::array<float, 12> gamut{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::array<float, 4>  offset{};
};

// Values are shared with the named operations in shaders/common.glsl.
enum class color_operation_e : int32_t
{
    none                        = 0,
    decode_rec709               = 1,
    encode_rec709               = 2,
    encode_rec709_premultiplied = 3,

    // External alpha representations convert to/from linear premultiplied RGB.
    decode_rec709_straight_alpha = 4,
    encode_rec709_straight_alpha = 5,
    decode_rec709_premultiplied  = 6,
    decode_rec709_ignore_alpha   = 7,
    encode_rec709_ignore_alpha   = 8,
    decode_srgb_premultiplied    = 9,
};

enum class alpha_mode_e
{
    ignore,
    straight,
    premultiplied,
};

// A/B interpolation space, distinct from compositing the result onto a target.
enum class blend_mode_e : uint8_t
{
    video,
    linear,
};

enum class compositing_e
{
    replace,
    source_over, // Sources use premultiplied alpha.
};

struct draw_s
{
    // Pixel-space destination; an empty extent means the whole destination.
    std::array<float, 4> destination{};
    std::array<float, 4> uv{0, 0, 1, 1};

    float         opacity{1};
    compositing_e compositing{compositing_e::source_over};

    color_operation_e transfer{color_operation_e::none};
    channel_order_e   input_order{channel_order_e::rgba};
    channel_order_e   output_order{channel_order_e::rgba};

    // Pixel-space x/y/width/height, intersected with the destination extent.
    std::optional<std::array<int32_t, 4>> clip{};
};

struct mix_s
{
    std::array<float, 4> a_destination{0, 0, 1, 1};
    std::array<float, 4> a_uv{0, 0, 1, 1};
    std::array<float, 4> b_destination{0, 0, 1, 1};
    std::array<float, 4> b_uv{0, 0, 1, 1};

    float                                 fraction{0.5f};
    blend_mode_e                          blend_mode{blend_mode_e::linear};
    compositing_e                         compositing{compositing_e::source_over};
    std::optional<std::array<int32_t, 4>> clip{};
};

class recording_s
{
    std::unique_ptr<detail::recording_state_s> state_;
    explicit recording_s(std::unique_ptr<detail::recording_state_s> state);
    friend class device_s;
    // GPU-only hardware comparison probe; no production recording changes.
    friend class detail::color_comparison_s;
    friend class recording_context_s;
    friend struct detail::presenter_state_s;
    friend class transfer::detail::cuda_transfer_s;
    friend class detail::dma_buf_export_s;
    friend class detail::dma_buf_copy_s;

  public:
    ~recording_s();
    recording_s(const recording_s&)            = delete;
    recording_s& operator=(const recording_s&) = delete;

    void upload(const buffer_s& source, const texture_s& destination, size_t row_stride = 0);
    void readback(const texture_s& source, const buffer_s& destination, size_t row_stride = 0);

    void unpack_v210(const buffer_s&          source,
                     const texture_s&         destination,
                     const color_transform_s& color,
                     size_t                   row_stride);
    void
    pack_v210(const texture_s& source, const buffer_s& destination, const color_transform_s& color, size_t row_stride);

    void copy(const buffer_s& source, const buffer_s& destination, size_t bytes);
    void generate_mip_maps(const texture_s& texture);
    void clear(const texture_s& target, std::array<float, 4> color = {});
    void draw(const texture_s& source, const texture_s& destination, const draw_s& parameters = {});
    void mix(const texture_s& a, const texture_s& b, const texture_s& destination, const mix_s& parameters = {});

    // Callbacks run on the submission worker only after native submission succeeds.
    // Destroying an unsubmitted recording releases the captured output leases.
    void on_submitted(std::function<void(completion_s)> publish);

    // Enqueue a GPU dependency without waiting for producer submission or completion
    // on this thread. The submission worker lets other contexts progress meanwhile.
    void wait_for(const completion_s& dependency);
    // Enqueues this recording; the ticket distinguishes submission from completion.
    completion_s submit();
};

// Independently owned command/descriptor pools. Recording never borrows capacity
// from another context, and queued/GPU uses retain each arena until retirement.
class recording_context_s
{
    std::shared_ptr<detail::recording_context_state_s> state_;
    explicit recording_context_s(std::shared_ptr<detail::recording_context_state_s> state);
    friend class device_s;
    friend struct detail::presenter_state_s;

  public:
    recording_context_s(recording_context_s&&) noexcept                    = default;
    recording_context_s&         operator=(recording_context_s&&) noexcept = default;
    std::unique_ptr<recording_s> try_record();
};

} // namespace miximus::gpu
