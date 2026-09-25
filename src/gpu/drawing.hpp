#pragma once

#include "color_transfer.hpp"
#include "geometry.hpp"
#include "recording.hpp"
#include "texture.hpp"

#include <stdexcept>

namespace miximus::gpu {

inline color_operation_e rec709_decode_operation(input_alpha_mode_e alpha)
{
    switch (alpha) {
        case input_alpha_mode_e::ignore:
            return color_operation_e::decode_rec709_ignore_alpha;
        case input_alpha_mode_e::straight:
            return color_operation_e::decode_rec709_straight_alpha;
        case input_alpha_mode_e::premultiplied:
            return color_operation_e::decode_rec709_premultiplied;
        case input_alpha_mode_e::straight_over_black:
            return color_operation_e::decode_rec709_straight_alpha_over_black;
    }

    throw std::invalid_argument("invalid alpha mode");
}

inline color_operation_e rec709_encode_operation(output_alpha_mode_e alpha)
{
    switch (alpha) {
        case output_alpha_mode_e::ignore:
            return color_operation_e::encode_rec709_ignore_alpha;
        case output_alpha_mode_e::straight:
            return color_operation_e::encode_rec709_straight_alpha;
        case output_alpha_mode_e::premultiplied:
            return color_operation_e::encode_rec709_premultiplied;
    }

    throw std::invalid_argument("invalid alpha mode");
}

struct texture_draw_options_s
{
    texture_draw_s         geometry{};
    double                 opacity{1};
    color_operation_e      transfer{color_operation_e::none};
    compositing_e          compositing{compositing_e::source_over};
    channel_order_e        output{channel_order_e::rgba};
    std::optional<recti_s> viewport{};
};

// Geometry stays normalized at the node boundary; the recorder receives pixels.
inline void draw_texture(recording_s&                  commands,
                         const texture_s*              source,
                         texture_s*                    target,
                         const texture_draw_options_s& options = {})
{
    if (!source || !target || options.geometry.destination.size.x == 0 || options.geometry.destination.size.y == 0) {
        return;
    }

    const auto bounds = options.viewport.value_or(recti_s{
        {0, 0},
        target->dimensions()
    });
    draw_s     draw;
    draw.destination  = {static_cast<float>(bounds.pos.x + options.geometry.destination.pos.x * bounds.size.x),
                         static_cast<float>(bounds.pos.y + options.geometry.destination.pos.y * bounds.size.y),
                         static_cast<float>(options.geometry.destination.size.x * bounds.size.x),
                         static_cast<float>(options.geometry.destination.size.y * bounds.size.y)};
    draw.uv           = {static_cast<float>(options.geometry.source.pos.x),
                         static_cast<float>(options.geometry.source.pos.y),
                         static_cast<float>(options.geometry.source.size.x),
                         static_cast<float>(options.geometry.source.size.y)};
    draw.opacity      = static_cast<float>(options.opacity);
    draw.compositing  = options.compositing;
    draw.transfer     = options.transfer;
    draw.input_order  = source->channel_order();
    draw.output_order = options.output;
    if (options.viewport) {
        draw.clip = {bounds.pos.x, bounds.pos.y, bounds.size.x, bounds.size.y};
    }

    commands.draw(*source, *target, draw);
}

enum class color_conversion_direction_e
{
    from_yuv,
    to_yuv,
};

color_transform_s
color_parameters(const color_conversion_s& conversion, const mat3& gamut, color_conversion_direction_e direction);

inline void mix_textures(recording_s&          commands,
                         const texture_s*      a,
                         const texture_s*      b,
                         texture_s*            target,
                         double                fraction,
                         const texture_draw_s& a_draw,
                         const texture_draw_s& b_draw,
                         blend_mode_e          blend_mode)
{
    if (!a || !b || !target) {
        return;
    }

    const auto rect = [](rect_s r) {
        return std::array<float, 4>{static_cast<float>(r.pos.x),
                                    static_cast<float>(r.pos.y),
                                    static_cast<float>(r.size.x),
                                    static_cast<float>(r.size.y)};
    };

    commands.mix(*a,
                 *b,
                 *target,
                 {.a_destination = rect(a_draw.destination),
                  .a_uv          = rect(a_draw.source),
                  .b_destination = rect(b_draw.destination),
                  .b_uv          = rect(b_draw.source),
                  .fraction      = static_cast<float>(fraction),
                  .blend_mode    = blend_mode});
}
} // namespace miximus::gpu
