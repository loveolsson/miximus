#pragma once
#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace miximus::gpu {

/** Linearly interpolate both components of a rectangle. */
[[nodiscard]] inline rect_s interpolate(rect_s from, rect_s to, double amount) noexcept
{
    return {
        .pos  = from.pos + ((to.pos - from.pos) * amount),
        .size = from.size + ((to.size - from.size) * amount),
    };
}

/** Round a floating-point vector to integer coordinates. */
[[nodiscard]] inline vec2i_t round_to_integer(vec2_t value) noexcept
{
    return {
        static_cast<int>(std::round(value.x)),
        static_cast<int>(std::round(value.y)),
    };
}

/** Round a floating-point rectangle to integer coordinates. */
[[nodiscard]] inline recti_s round_to_integer(rect_s rect) noexcept
{
    return {
        .pos  = round_to_integer(rect.pos),
        .size = round_to_integer(rect.size),
    };
}

/** Convert a pixel-space vector into normalized coordinates for a target. */
[[nodiscard]] inline vec2_t pixels_to_normalized(vec2_t value, vec2i_t target_dimensions)
{
    if (target_dimensions.x <= 0 || target_dimensions.y <= 0) {
        throw std::invalid_argument("coordinate conversion requires positive target dimensions");
    }

    return value / vec2_t(target_dimensions);
}

enum class aspect_scale_e
{
    contain,
    cover,
};

enum class fill_mode_e : uint8_t
{
    scale,
    fill,
    contain,
};

struct texture_draw_s
{
    rect_s destination{};
    rect_s source{};
};

/**
 * Center content with its aspect ratio preserved inside `bounds`.
 *
 * Bounds are expressed in normalized coordinates relative to a target whose
 * pixel dimensions are `target_dimensions`. `cover` may return a rectangle
 * larger than bounds and therefore requires clipping by the caller.
 */
[[nodiscard]] inline rect_s
scale_rect_to_aspect(rect_s bounds, vec2i_t content_dimensions, vec2i_t target_dimensions, aspect_scale_e mode)
{
    if (content_dimensions.x <= 0 || content_dimensions.y <= 0 || target_dimensions.x <= 0 ||
        target_dimensions.y <= 0 || bounds.size.x <= 0 || bounds.size.y <= 0) {
        throw std::invalid_argument("aspect scaling requires positive dimensions");
    }

    const double content_aspect = static_cast<double>(content_dimensions.x) / content_dimensions.y;
    const double bounds_aspect  = (bounds.size.x * target_dimensions.x) / (bounds.size.y * target_dimensions.y);

    auto size = bounds.size;
    if (mode == aspect_scale_e::contain) {
        if (content_aspect > bounds_aspect) {
            size.y *= bounds_aspect / content_aspect;
        } else {
            size.x *= content_aspect / bounds_aspect;
        }
    } else if (content_aspect > bounds_aspect) {
        size.x *= content_aspect / bounds_aspect;
    } else {
        size.y *= bounds_aspect / content_aspect;
    }

    return {
        .pos  = bounds.pos + ((bounds.size - size) * 0.5),
        .size = size,
    };
}

[[nodiscard]] inline rect_s contain_aspect_ratio(rect_s bounds, vec2i_t content_dimensions, vec2i_t target_dimensions)
{
    return scale_rect_to_aspect(bounds, content_dimensions, target_dimensions, aspect_scale_e::contain);
}

[[nodiscard]] inline rect_s cover_aspect_ratio(rect_s bounds, vec2i_t content_dimensions, vec2i_t target_dimensions)
{
    return scale_rect_to_aspect(bounds, content_dimensions, target_dimensions, aspect_scale_e::cover);
}

/** Calculate destination placement and source cropping for drawing a texture into bounds. */
[[nodiscard]] inline texture_draw_s
calculate_texture_draw(rect_s bounds, vec2i_t content_dimensions, vec2i_t target_dimensions, fill_mode_e fill_mode)
{
    if (fill_mode == fill_mode_e::scale || bounds.size.x == 0.0 || bounds.size.y == 0.0) {
        return {.destination = bounds};
    }

    const bool flip_x = bounds.size.x < 0.0;
    const bool flip_y = bounds.size.y < 0.0;
    if (flip_x) {
        bounds.pos.x += bounds.size.x;
        bounds.size.x *= -1.0;
    }
    if (flip_y) {
        bounds.pos.y += bounds.size.y;
        bounds.size.y *= -1.0;
    }

    texture_draw_s result;
    if (fill_mode == fill_mode_e::contain) {
        result.destination = contain_aspect_ratio(bounds, content_dimensions, target_dimensions);
    } else if (fill_mode == fill_mode_e::fill) {
        const auto covered = cover_aspect_ratio(bounds, content_dimensions, target_dimensions);
        result.destination = bounds;
        result.source      = {
                 .pos  = (bounds.pos - covered.pos) / covered.size,
                 .size = bounds.size / covered.size,
        };
    } else {
        throw std::invalid_argument("unknown texture fill mode");
    }

    if (flip_x) {
        result.destination.pos.x += result.destination.size.x;
        result.destination.size.x *= -1.0;
    }
    if (flip_y) {
        result.destination.pos.y += result.destination.size.y;
        result.destination.size.y *= -1.0;
    }
    return result;
}

/** Convert a normalized rectangle into a pixel rectangle for a target. */
[[nodiscard]] inline recti_s normalized_to_pixel_rect(rect_s rect, vec2i_t target_dimensions)
{
    if (target_dimensions.x <= 0 || target_dimensions.y <= 0) {
        throw std::invalid_argument("coordinate conversion requires positive target dimensions");
    }

    const auto width  = static_cast<int>(std::round(rect.size.x * target_dimensions.x));
    const auto height = static_cast<int>(std::round(rect.size.y * target_dimensions.y));

    return {
        .pos =
            {
                  static_cast<int>(std::round(rect.pos.x * target_dimensions.x)),
                  static_cast<int>(std::round(rect.pos.y * target_dimensions.y)),
                  },
        .size =
            {
                  std::max(0, width),
                  std::max(0, height),
                  },
    };
}

} // namespace miximus::gpu
