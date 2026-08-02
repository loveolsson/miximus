#include "surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace miximus::render {

#ifdef _MSC_VER
#define MIXIMUS_SURFACE_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MIXIMUS_SURFACE_FORCE_INLINE inline __attribute__((always_inline))
#else
#define MIXIMUS_SURFACE_FORCE_INLINE inline
#endif

surface_s::surface_s(gpu::vec2i_t dimensions, std::span<std::byte> storage)
    : dimensions_(dimensions)
{
    if (dimensions_.x < 0 || dimensions_.y < 0) {
        throw std::invalid_argument("surface dimensions must not be negative");
    }
    if (dimensions_.x != 0 &&
        static_cast<size_t>(dimensions_.y) > std::numeric_limits<size_t>::max() / static_cast<size_t>(dimensions_.x)) {
        throw std::length_error("surface pixel count overflows size_t");
    }
    const auto pixel_count = static_cast<size_t>(dimensions_.x) * static_cast<size_t>(dimensions_.y);
    if (pixel_count > std::numeric_limits<size_t>::max() / sizeof(pixel_t) ||
        storage.size() < pixel_count * sizeof(pixel_t)) {
        throw std::invalid_argument("surface storage is smaller than its dimensions");
    }
    if (pixel_count > 0 && storage.data() == nullptr) {
        throw std::invalid_argument("surface storage must not be null");
    }
    pixels_ = {reinterpret_cast<pixel_t*>(storage.data()), pixel_count};
}

surface_s::surface_s(gpu::vec2i_t dimensions, std::span<pixel_t> pixels)
    : surface_s(dimensions, std::as_writable_bytes(pixels))
{
}

void surface_s::clear(pixel_t color) noexcept { std::ranges::fill(pixels_, color); }

namespace {
struct clipped_rect_s
{
    gpu::vec2i_t begin;
    gpu::vec2i_t end;
};

gpu::recti_s make_rect(gpu::vec2i_t position, gpu::vec2i_t size) noexcept { return {.pos = position, .size = size}; }

std::optional<clipped_rect_s> clip_rect(gpu::recti_s rect, gpu::vec2i_t dimensions) noexcept
{
    if (rect.size.x <= 0 || rect.size.y <= 0) {
        return std::nullopt;
    }

    const auto end_x = static_cast<int64_t>(rect.pos.x) + rect.size.x;
    const auto end_y = static_cast<int64_t>(rect.pos.y) + rect.size.y;
    const auto begin = gpu::vec2i_t{std::max(rect.pos.x, 0), std::max(rect.pos.y, 0)};
    const auto end   = gpu::vec2i_t{
        static_cast<int>(std::min<int64_t>(end_x, dimensions.x)),
        static_cast<int>(std::min<int64_t>(end_y, dimensions.y)),
    };

    if (begin.x >= end.x || begin.y >= end.y) {
        return std::nullopt;
    }
    return clipped_rect_s{.begin = begin, .end = end};
}

template <typename Op>
void raster_operation(surface_s::pixel_t* dst_ptr, gpu::vec2i_t dst_dim, gpu::recti_s rect, Op op) noexcept
{
    const auto clipped = clip_rect(rect, dst_dim);
    if (!clipped.has_value()) {
        return;
    }

    auto* row = dst_ptr + (static_cast<size_t>(clipped->begin.y) * static_cast<size_t>(dst_dim.x));
    for (int y = clipped->begin.y; y < clipped->end.y; ++y) {
        for (int x = clipped->begin.x; x < clipped->end.x; ++x) {
            op(gpu::vec2i_t{x, y}, &row[x]);
        }
        row += dst_dim.x;
    }
}

template <typename CoverageOp, typename PixelOp>
void coverage_operation(surface_s::pixel_t* dst_ptr,
                        gpu::vec2i_t        dst_dim,
                        gpu::recti_s        bounds,
                        CoverageOp          coverage_op,
                        PixelOp             pixel_op) noexcept
{
    raster_operation(dst_ptr, dst_dim, bounds, [coverage_op, pixel_op](auto position, auto* pixel) {
        const double coverage = coverage_op(position);
        if (coverage > 0.0) {
            pixel_op(coverage, pixel);
        }
    });
}

template <typename SrcT, typename Op>
void copy_operation(const strided_image_view_s<SrcT>& source,
                    surface_s::pixel_t*               dst_ptr,
                    gpu::vec2i_t                      dst_dim,
                    gpu::vec2i_t                      pos,
                    Op                                op) noexcept
{
    const auto src_dim = source.dimensions();

    const auto src_x = std::max<int64_t>(0, -static_cast<int64_t>(pos.x));
    const auto src_y = std::max<int64_t>(0, -static_cast<int64_t>(pos.y));
    const auto dst_x = std::max<int64_t>(0, pos.x);
    const auto dst_y = std::max<int64_t>(0, pos.y);

    const auto width  = std::min(static_cast<int64_t>(src_dim.x) - src_x, static_cast<int64_t>(dst_dim.x) - dst_x);
    const auto height = std::min(static_cast<int64_t>(src_dim.y) - src_y, static_cast<int64_t>(dst_dim.y) - dst_y);

    if (width <= 0 || height <= 0) {
        return;
    }

    const auto* src_row = reinterpret_cast<const std::byte*>(source.row(static_cast<size_t>(src_y)).data());
    auto*       dst_row = dst_ptr + (dst_y * dst_dim.x) + dst_x;

    for (int64_t y = 0; y < height; ++y) {
        const auto* typed_src_row = reinterpret_cast<const SrcT*>(src_row);

        for (int64_t x = 0; x < width; ++x) {
            op(typed_src_row[src_x + x], &dst_row[x]);
        }

        src_row += source.row_stride_bytes();
        dst_row += dst_dim.x;
    }
}

uint8_t interpolate_channel(uint8_t from, uint8_t to, int64_t numerator, int64_t denominator) noexcept
{
    if (denominator <= 0) {
        return from;
    }
    const auto value = (static_cast<int64_t>(from) * (denominator - numerator)) +
                       (static_cast<int64_t>(to) * numerator) + (denominator / 2);
    return static_cast<uint8_t>(value / denominator);
}

surface_s::pixel_t
interpolate(surface_s::pixel_t from, surface_s::pixel_t to, int64_t numerator, int64_t denominator) noexcept
{
    return {
        interpolate_channel(from.r, to.r, numerator, denominator),
        interpolate_channel(from.g, to.g, numerator, denominator),
        interpolate_channel(from.b, to.b, numerator, denominator),
        interpolate_channel(from.a, to.a, numerator, denominator),
    };
}

uint8_t multiply_channel(uint8_t lhs, uint8_t rhs) noexcept
{
    constexpr uint32_t channel_max = 255;
    return static_cast<uint8_t>(((static_cast<uint32_t>(lhs) * rhs) + (channel_max / 2)) / channel_max);
}

surface_s::pixel_t premultiply(straight_rgba_pixel_s source) noexcept
{
    return {
        multiply_channel(source.r, source.a),
        multiply_channel(source.g, source.a),
        multiply_channel(source.b, source.a),
        source.a,
    };
}

void composite_source_over(surface_s::pixel_t source, surface_s::pixel_t* destination) noexcept
{
    constexpr uint8_t channel_max = 255;
    const uint8_t     inverse     = channel_max - source.a;

    const auto composite_channel = [inverse](uint8_t source_channel, uint8_t destination_channel) {
        return static_cast<uint8_t>(source_channel + multiply_channel(destination_channel, inverse));
    };

    destination->r = composite_channel(source.r, destination->r);
    destination->g = composite_channel(source.g, destination->g);
    destination->b = composite_channel(source.b, destination->b);
    destination->a = composite_channel(source.a, destination->a);
}

const std::array<uint8_t, 256>& srgb_to_linear_lut() noexcept
{
    static const auto lut = [] {
        std::array<uint8_t, 256> result{};
        for (size_t i = 0; i < result.size(); ++i) {
            const double encoded = static_cast<double>(i) / 255.0;
            const double linear  = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
            result[i]            = static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 255.0));
        }
        return result;
    }();
    return lut;
}

uint8_t unpremultiply_channel(uint8_t channel, uint8_t alpha) noexcept
{
    if (alpha == 0) {
        return 0;
    }
    return static_cast<uint8_t>(
        std::min<uint32_t>(255, ((static_cast<uint32_t>(channel) * 255) + (alpha / 2)) / alpha));
}

surface_s::pixel_t convert_srgb_premultiplied_bgra(srgb_premultiplied_bgra_pixel_s source) noexcept
{
    const auto& lut = srgb_to_linear_lut();
    return {
        multiply_channel(lut[unpremultiply_channel(source.r, source.a)], source.a),
        multiply_channel(lut[unpremultiply_channel(source.g, source.a)], source.a),
        multiply_channel(lut[unpremultiply_channel(source.b, source.a)], source.a),
        source.a,
    };
}

struct replace_coverage_s
{
    surface_s::pixel_t color;

    MIXIMUS_SURFACE_FORCE_INLINE void operator()(double coverage, surface_s::pixel_t* destination) const noexcept
    {
        if (coverage >= 1.0) {
            *destination = color;
            return;
        }

        const auto mix_channel = [coverage](uint8_t from, uint8_t to) {
            return static_cast<uint8_t>(std::lround(from + ((static_cast<int>(to) - from) * coverage)));
        };
        destination->r = mix_channel(destination->r, color.r);
        destination->g = mix_channel(destination->g, color.g);
        destination->b = mix_channel(destination->b, color.b);
        destination->a = mix_channel(destination->a, color.a);
    }
};

struct source_over_coverage_s
{
    straight_rgba_pixel_s color;

    void operator()(double coverage, surface_s::pixel_t* destination) const noexcept
    {
        auto source = color;
        source.a    = static_cast<uint8_t>(std::lround(color.a * coverage));
        composite_source_over(premultiply(source), destination);
    }
};

bool clip_line(gpu::vec2i_t dimensions, gpu::vec2i_t* from, gpu::vec2i_t* to) noexcept
{
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        return false;
    }

    double     x0 = from->x;
    double     y0 = from->y;
    double     x1 = to->x;
    double     y1 = to->y;
    const auto dx = x1 - x0;
    const auto dy = y1 - y0;
    double     first{};
    double     last{1.0};

    const auto clip_boundary = [&first, &last](double p, double q) {
        if (p == 0.0) {
            return q >= 0.0;
        }
        const auto ratio = q / p;
        if (p < 0.0) {
            if (ratio > last) {
                return false;
            }
            first = std::max(first, ratio);
        } else {
            if (ratio < first) {
                return false;
            }
            last = std::min(last, ratio);
        }
        return true;
    };

    if (!clip_boundary(-dx, x0) || !clip_boundary(dx, (dimensions.x - 1) - x0) || !clip_boundary(-dy, y0) ||
        !clip_boundary(dy, (dimensions.y - 1) - y0)) {
        return false;
    }

    *from = {
        static_cast<int>(std::lround(x0 + (first * dx))),
        static_cast<int>(std::lround(y0 + (first * dy))),
    };
    *to = {
        static_cast<int>(std::lround(x0 + (last * dx))),
        static_cast<int>(std::lround(y0 + (last * dy))),
    };
    return true;
}

double normalized_ellipse_distance_squared(double x, double y, double radius_x, double radius_y) noexcept
{
    return ((x * x) / (radius_x * radius_x)) + ((y * y) / (radius_y * radius_y));
}

struct ellipse_coverage_s
{
    double center_x;
    double center_y;
    double radius_x;
    double radius_y;

    ellipse_coverage_s(double center_x_, double center_y_, double radius_x_, double radius_y_)
        : center_x(center_x_)
        , center_y(center_y_)
        , radius_x(radius_x_)
        , radius_y(radius_y_)
    {
    }

    explicit ellipse_coverage_s(gpu::recti_s bounds)
        : ellipse_coverage_s(bounds.pos.x + (bounds.size.x / 2.0),
                             bounds.pos.y + (bounds.size.y / 2.0),
                             bounds.size.x / 2.0,
                             bounds.size.y / 2.0)
    {
    }

    ellipse_coverage_s inset(double amount) const noexcept
    {
        return {center_x, center_y, radius_x - amount, radius_y - amount};
    }

    MIXIMUS_SURFACE_FORCE_INLINE double operator()(gpu::vec2i_t position) const noexcept
    {
        const double x = position.x + 0.5 - center_x;
        const double y = position.y + 0.5 - center_y;

        const double inner_radius_x = radius_x - 0.5;
        const double inner_radius_y = radius_y - 0.5;
        if (inner_radius_x > 0.0 && inner_radius_y > 0.0 &&
            normalized_ellipse_distance_squared(x, y, inner_radius_x, inner_radius_y) <= 1.0) {
            return 1.0;
        }
        if (normalized_ellipse_distance_squared(x, y, radius_x + 0.5, radius_y + 0.5) >= 1.0) {
            return 0.0;
        }

        const double implicit_value = normalized_ellipse_distance_squared(x, y, radius_x, radius_y) - 1.0;
        const double gradient_x     = (2.0 * x) / (radius_x * radius_x);
        const double gradient_y     = (2.0 * y) / (radius_y * radius_y);
        const double gradient       = std::hypot(gradient_x, gradient_y);
        if (gradient == 0.0) {
            return 1.0;
        }
        return std::clamp(0.5 - (implicit_value / gradient), 0.0, 1.0);
    }
};

struct pill_coverage_s
{
    gpu::recti_s bounds;

    MIXIMUS_SURFACE_FORCE_INLINE double operator()(gpu::vec2i_t position) const noexcept
    {
        const double pixel_x = position.x + 0.5;
        const double pixel_y = position.y + 0.5;

        double radius{};
        double delta_x{};
        double delta_y{};
        if (bounds.size.x >= bounds.size.y) {
            radius                 = bounds.size.y / 2.0;
            const double center_y  = bounds.pos.y + radius;
            const double cap_begin = bounds.pos.x + radius;
            const double cap_end   = bounds.pos.x + bounds.size.x - radius;
            delta_x                = pixel_x - std::clamp(pixel_x, cap_begin, cap_end);
            delta_y                = pixel_y - center_y;
        } else {
            radius                 = bounds.size.x / 2.0;
            const double center_x  = bounds.pos.x + radius;
            const double cap_begin = bounds.pos.y + radius;
            const double cap_end   = bounds.pos.y + bounds.size.y - radius;
            delta_x                = pixel_x - center_x;
            delta_y                = pixel_y - std::clamp(pixel_y, cap_begin, cap_end);
        }

        const double distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
        const double inner_radius     = radius - 0.5;
        if (inner_radius > 0.0 && distance_squared <= inner_radius * inner_radius) {
            return 1.0;
        }
        const double outer_radius = radius + 0.5;
        if (distance_squared >= outer_radius * outer_radius) {
            return 0.0;
        }
        return std::clamp(outer_radius - std::sqrt(distance_squared), 0.0, 1.0);
    }
};

template <typename OuterCoverage, typename InnerCoverage>
struct difference_coverage_s
{
    OuterCoverage outer;
    InnerCoverage inner;

    double operator()(gpu::vec2i_t position) const noexcept
    {
        return std::clamp(outer(position) - inner(position), 0.0, 1.0);
    }
};
} // namespace

#undef MIXIMUS_SURFACE_FORCE_INLINE

void surface_s::source_over(const strided_image_view_s<straight_rgba_pixel_s>& source, gpu::vec2i_t position) noexcept
{
    copy_operation(source, data(), dimensions_, position, [](const auto& source_pixel, auto* destination) {
        composite_source_over(premultiply(source_pixel), destination);
    });
}

void surface_s::source_over(const strided_image_view_s<srgb_premultiplied_bgra_pixel_s>& source,
                            gpu::vec2i_t                                                 position) noexcept
{
    copy_operation(source, data(), dimensions_, position, [](const auto& source_pixel, auto* destination) {
        composite_source_over(convert_srgb_premultiplied_bgra(source_pixel), destination);
    });
}

void surface_s::source_over(const strided_image_view_s<coverage_pixel_t>& source,
                            gpu::vec2i_t                                  position,
                            straight_rgba_pixel_s                         color) noexcept
{
    copy_operation(source, data(), dimensions_, position, [color](auto coverage, auto* destination) {
        source_over_coverage_s{color}(static_cast<double>(coverage) / 255.0, destination);
    });
}

void surface_s::source_over(gpu::recti_s rect, straight_rgba_pixel_s color) noexcept
{
    const auto source = premultiply(color);
    raster_operation(data(), dimensions_, rect, [source](gpu::vec2i_t, auto* destination) {
        composite_source_over(source, destination);
    });
}

void surface_s::source_over_ellipse(gpu::recti_s bounds, straight_rgba_pixel_s color) noexcept
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    coverage_operation(data(), dimensions_, bounds, ellipse_coverage_s{bounds}, source_over_coverage_s{color});
}

void surface_s::fill(gpu::recti_s rect, pixel_t color) noexcept
{
    const auto clipped = clip_rect(rect, dimensions_);
    if (!clipped.has_value()) {
        return;
    }

    auto* row = data() + (static_cast<size_t>(clipped->begin.y) * static_cast<size_t>(dimensions_.x));
    for (int y = clipped->begin.y; y < clipped->end.y; ++y) {
        std::fill(row + clipped->begin.x, row + clipped->end.x, color);
        row += dimensions_.x;
    }
}

void surface_s::draw_rect(gpu::recti_s rect, pixel_t color, int thickness) noexcept
{
    if (thickness <= 0 || rect.size.x <= 0 || rect.size.y <= 0) {
        return;
    }

    const int horizontal = std::min(thickness, rect.size.y);
    const int vertical   = std::min(thickness, rect.size.x);
    fill(make_rect(rect.pos, {rect.size.x, horizontal}), color);
    fill(make_rect({rect.pos.x, rect.pos.y + rect.size.y - horizontal}, {rect.size.x, horizontal}), color);
    fill(make_rect(rect.pos, {vertical, rect.size.y}), color);
    fill(make_rect({rect.pos.x + rect.size.x - vertical, rect.pos.y}, {vertical, rect.size.y}), color);
}

void surface_s::draw_line(gpu::vec2i_t from, gpu::vec2i_t to, pixel_t color, int thickness) noexcept
{
    if (thickness <= 0 || !clip_line(dimensions_, &from, &to)) {
        return;
    }

    const int radius = (thickness - 1) / 2;
    int       dx     = std::abs(to.x - from.x);
    int       sx     = from.x < to.x ? 1 : -1;
    int       dy     = -std::abs(to.y - from.y);
    int       sy     = from.y < to.y ? 1 : -1;
    int       error  = dx + dy;

    while (true) {
        fill(make_rect({from.x - radius, from.y - radius}, {thickness, thickness}), color);
        if (from == to) {
            break;
        }
        const int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            from.x += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            from.y += sy;
        }
    }
}

void surface_s::fill_ellipse(gpu::recti_s bounds, pixel_t color) noexcept
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    coverage_operation(data(), dimensions_, bounds, ellipse_coverage_s{bounds}, replace_coverage_s{color});
}

void surface_s::draw_ellipse(gpu::recti_s bounds, pixel_t color, int thickness) noexcept
{
    if (thickness <= 0 || bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    const int minimum_size = std::min(bounds.size.x, bounds.size.y);
    if (thickness >= (minimum_size / 2) + (minimum_size % 2)) {
        fill_ellipse(bounds, color);
        return;
    }

    const auto outer = ellipse_coverage_s{bounds};
    coverage_operation(data(),
                       dimensions_,
                       bounds,
                       difference_coverage_s{.outer = outer, .inner = outer.inset(thickness)},
                       replace_coverage_s{color});
}

void surface_s::fill_circle(gpu::vec2i_t center, int radius, pixel_t color) noexcept
{
    if (radius <= 0) {
        return;
    }
    fill_ellipse(make_rect(center - gpu::vec2i_t{radius}, gpu::vec2i_t{radius * 2}), color);
}

void surface_s::draw_circle(gpu::vec2i_t center, int radius, pixel_t color, int thickness) noexcept
{
    if (radius <= 0) {
        return;
    }
    draw_ellipse(make_rect(center - gpu::vec2i_t{radius}, gpu::vec2i_t{radius * 2}), color, thickness);
}

void surface_s::fill_pill(gpu::recti_s bounds, pixel_t color) noexcept
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    coverage_operation(data(), dimensions_, bounds, pill_coverage_s{bounds}, replace_coverage_s{color});
}

void surface_s::draw_pill(gpu::recti_s bounds, pixel_t color, int thickness) noexcept
{
    if (thickness <= 0 || bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    const int minimum_size = std::min(bounds.size.x, bounds.size.y);
    if (thickness >= (minimum_size / 2) + (minimum_size % 2)) {
        fill_pill(bounds, color);
        return;
    }

    const gpu::recti_s inner_bounds{
        .pos  = bounds.pos + gpu::vec2i_t{thickness},
        .size = bounds.size - gpu::vec2i_t{thickness * 2},
    };
    coverage_operation(data(),
                       dimensions_,
                       bounds,
                       difference_coverage_s{
                           .outer = pill_coverage_s{bounds},
                           .inner = pill_coverage_s{inner_bounds},
                       },
                       replace_coverage_s{color});
}

void surface_s::horizontal_gradient(gpu::recti_s rect, pixel_t left, pixel_t right) noexcept
{
    raster_operation(data(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        *dst = interpolate(left, right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
    });
}

void surface_s::vertical_gradient(gpu::recti_s rect, pixel_t top, pixel_t bottom) noexcept
{
    raster_operation(data(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        *dst = interpolate(top, bottom, pos.y - rect.pos.y, std::max(rect.size.y - 1, 1));
    });
}

void surface_s::bilinear_gradient(gpu::recti_s rect,
                                  pixel_t      top_left,
                                  pixel_t      top_right,
                                  pixel_t      bottom_left,
                                  pixel_t      bottom_right) noexcept
{
    raster_operation(data(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        const auto top    = interpolate(top_left, top_right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
        const auto bottom = interpolate(bottom_left, bottom_right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
        *dst              = interpolate(top, bottom, pos.y - rect.pos.y, std::max(rect.size.y - 1, 1));
    });
}

void surface_s::checkerboard(gpu::recti_s rect, gpu::vec2i_t cell_size, pixel_t first, pixel_t second) noexcept
{
    if (cell_size.x <= 0 || cell_size.y <= 0) {
        return;
    }
    raster_operation(data(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        const int cell_x = (pos.x - rect.pos.x) / cell_size.x;
        const int cell_y = (pos.y - rect.pos.y) / cell_size.y;
        *dst             = ((cell_x + cell_y) & 1) == 0 ? first : second;
    });
}

void surface_s::draw_grid(gpu::recti_s rect, gpu::vec2i_t spacing, pixel_t color, int thickness) noexcept
{
    if (spacing.x <= 0 || spacing.y <= 0) {
        return;
    }
    if (thickness <= 0) {
        return;
    }

    const auto clipped = clip_rect(rect, dimensions_);
    if (!clipped.has_value()) {
        return;
    }

    const auto x_offset = static_cast<int64_t>(clipped->begin.x) - rect.pos.x;
    const auto y_offset = static_cast<int64_t>(clipped->begin.y) - rect.pos.y;
    auto       x        = static_cast<int64_t>(rect.pos.x) + (((x_offset + spacing.x - 1) / spacing.x) * spacing.x);
    auto       y        = static_cast<int64_t>(rect.pos.y) + (((y_offset + spacing.y - 1) / spacing.y) * spacing.y);

    for (; x < clipped->end.x; x += spacing.x) {
        fill(make_rect({static_cast<int>(x), clipped->begin.y}, {thickness, clipped->end.y - clipped->begin.y}), color);
    }
    for (; y < clipped->end.y; y += spacing.y) {
        fill(make_rect({clipped->begin.x, static_cast<int>(y)}, {clipped->end.x - clipped->begin.x, thickness}), color);
    }
}

} // namespace miximus::render
