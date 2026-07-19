#pragma once
#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace miximus::gpu {

/** Linearly interpolate both components of a rectangle. */
[[nodiscard]] inline rect_s interpolate(rect_s from, rect_s to, double amount)
{
    return {
        .pos  = from.pos + ((to.pos - from.pos) * amount),
        .size = from.size + ((to.size - from.size) * amount),
    };
}

/** Round a floating-point vector to integer coordinates. */
[[nodiscard]] inline vec2i_t round_to_integer(vec2_t value)
{
    return {
        static_cast<int>(std::round(value.x)),
        static_cast<int>(std::round(value.y)),
    };
}

/** Round a floating-point rectangle to integer coordinates. */
[[nodiscard]] inline recti_s round_to_integer(rect_s rect)
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
