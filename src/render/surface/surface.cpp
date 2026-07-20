#include "surface.hpp"

#include "static_files/files.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <glm/glm.hpp>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::render {

surface_s::surface_s(gpu::vec2i_t dim, rgba_pixel_t* ptr)
    : dimensions_(dim)
    , ptr_(ptr)
{
    if (dimensions_.x < 0 || dimensions_.y < 0) {
        throw std::invalid_argument("surface dimensions must not be negative");
    }
    if (ptr_ == nullptr) {
        throw std::invalid_argument("surface pointer must not be null");
    }
}

bool surface_s::contains(gpu::vec2i_t position) const
{
    return position.x >= 0 && position.y >= 0 && position.x < dimensions_.x && position.y < dimensions_.y;
}

surface_s::rgba_pixel_t& surface_s::pixel(gpu::vec2i_t position)
{
    if (!contains(position)) {
        throw std::out_of_range("surface pixel is outside its dimensions");
    }
    return ptr_[(static_cast<size_t>(position.y) * static_cast<size_t>(dimensions_.x)) +
                static_cast<size_t>(position.x)];
}

const surface_s::rgba_pixel_t& surface_s::pixel(gpu::vec2i_t position) const
{
    if (!contains(position)) {
        throw std::out_of_range("surface pixel is outside its dimensions");
    }
    return ptr_[(static_cast<size_t>(position.y) * static_cast<size_t>(dimensions_.x)) +
                static_cast<size_t>(position.x)];
}

void surface_s::clear(const rgba_pixel_t& color)
{
    std::fill(ptr(), ptr() + (static_cast<size_t>(dimensions_.x) * static_cast<size_t>(dimensions_.y)), color);
}

namespace {
struct clipped_rect_s
{
    gpu::vec2i_t begin;
    gpu::vec2i_t end;
};

struct asset_s
{
    gpu::vec2i_t                         dimensions;
    std::vector<surface_s::rgba_pixel_t> pixels;
};

struct stbi_deleter_s
{
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

uint8_t rec709_to_linear(uint8_t value)
{
    const double encoded = static_cast<double>(value) / 255.0;
    const double linear  = encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
    return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 255.0));
}

std::shared_ptr<const asset_s> load_asset(std::string_view resource_path)
{
    static std::mutex                                                         mutex;
    static std::map<std::string, std::shared_ptr<const asset_s>, std::less<>> assets;

    const std::scoped_lock lock(mutex);
    if (const auto existing = assets.find(resource_path); existing != assets.end()) {
        return existing->second;
    }

    const auto file_data = static_files::get_resource_files().get_file_or_throw(resource_path).unzip();
    if (file_data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error(std::format("Image {} is too large to decode", resource_path));
    }

    gpu::vec2i_t                                   dimensions;
    int                                            source_channels{};
    const std::unique_ptr<stbi_uc, stbi_deleter_s> decoded(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file_data.data()),
                              static_cast<int>(file_data.size()),
                              &dimensions.x,
                              &dimensions.y,
                              &source_channels,
                              4));
    if (!decoded) {
        throw std::runtime_error(std::format("Failed to load image {}", resource_path));
    }
    constexpr size_t channel_count = 4;
    if (dimensions.x <= 0 || dimensions.y <= 0 ||
        static_cast<size_t>(dimensions.x) > std::numeric_limits<size_t>::max() / static_cast<size_t>(dimensions.y) ||
        (static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y)) >
            std::numeric_limits<size_t>::max() / channel_count) {
        throw std::length_error(std::format("Image {} has invalid dimensions", resource_path));
    }

    const auto pixel_count = static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y);
    auto       asset       = std::make_shared<asset_s>(asset_s{
                    .dimensions = dimensions,
                    .pixels     = std::vector<surface_s::rgba_pixel_t>(pixel_count),
    });
    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t offset = i * channel_count;
        asset->pixels[i]    = {
            rec709_to_linear(decoded.get()[offset]),
            rec709_to_linear(decoded.get()[offset + 1]),
            rec709_to_linear(decoded.get()[offset + 2]),
            decoded.get()[offset + 3],
        };
    }

    const auto result = assets.emplace(std::string(resource_path), std::move(asset));
    return result.first->second;
}

gpu::recti_s make_rect(gpu::vec2i_t position, gpu::vec2i_t size) { return {.pos = position, .size = size}; }

std::optional<clipped_rect_s> clip_rect(gpu::recti_s rect, gpu::vec2i_t dimensions)
{
    if (rect.size.x < 0 || rect.size.y < 0) {
        throw std::invalid_argument("rectangle size must not be negative");
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
void raster_operation(surface_s::rgba_pixel_t* dst_ptr, gpu::vec2i_t dst_dim, gpu::recti_s rect, Op op)
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

template <typename SrcT, typename Op>
void copy_operation(const SrcT*              src_ptr,
                    gpu::vec2i_t             src_dim,
                    size_t                   src_pitch,
                    surface_s::rgba_pixel_t* dst_ptr,
                    gpu::vec2i_t             dst_dim,
                    gpu::vec2i_t             pos,
                    Op                       op)
{
    if (src_ptr == nullptr && src_dim.x > 0 && src_dim.y > 0) {
        throw std::invalid_argument("copy_operation called with a null source");
    }
    if (src_dim.x < 0 || src_dim.y < 0 || dst_dim.x < 0 || dst_dim.y < 0) {
        throw std::length_error("copy_operation called with invalid dimensions");
    }
    if (src_pitch < static_cast<size_t>(src_dim.x) * sizeof(SrcT)) {
        throw std::length_error("copy_operation called with invalid pitch");
    }

    const auto src_x = std::max<int64_t>(0, -static_cast<int64_t>(pos.x));
    const auto src_y = std::max<int64_t>(0, -static_cast<int64_t>(pos.y));
    const auto dst_x = std::max<int64_t>(0, pos.x);
    const auto dst_y = std::max<int64_t>(0, pos.y);

    const auto width  = std::min(static_cast<int64_t>(src_dim.x) - src_x, static_cast<int64_t>(dst_dim.x) - dst_x);
    const auto height = std::min(static_cast<int64_t>(src_dim.y) - src_y, static_cast<int64_t>(dst_dim.y) - dst_y);

    if (width <= 0 || height <= 0) {
        return;
    }

    const auto* src_row = reinterpret_cast<const char*>(src_ptr) + (src_y * src_pitch);
    auto*       dst_row = dst_ptr + (dst_y * dst_dim.x) + dst_x;

    for (int64_t y = 0; y < height; ++y) {
        const auto* typed_src_row = reinterpret_cast<const SrcT*>(src_row) + src_x;

        for (int64_t x = 0; x < width; ++x) {
            op(typed_src_row[x], &dst_row[x]);
        }

        src_row += src_pitch;
        dst_row += dst_dim.x;
    }
}

surface_s::rgba_pixel_t interpolate(const surface_s::rgba_pixel_t& from,
                                    const surface_s::rgba_pixel_t& to,
                                    int64_t                        numerator,
                                    int64_t                        denominator)
{
    if (denominator <= 0) {
        return from;
    }

    glm::i64vec4 value = glm::i64vec4(from) * (denominator - numerator);
    value += glm::i64vec4(to) * numerator;
    value += denominator / 2;
    return {value / denominator};
}

void source_over_pixel(const surface_s::rgba_pixel_t& source, surface_s::rgba_pixel_t* destination)
{
    constexpr int channel_max     = 255;
    const int     source_alpha    = source.a;
    const int     inverse         = channel_max - source_alpha;
    const auto    source_rgb      = glm::ivec3(source) * source_alpha;
    const auto    destination_rgb = glm::ivec3(*destination) * inverse;
    const auto    output_rgb      = (source_rgb + destination_rgb + (channel_max / 2)) / channel_max;
    const int     output_alpha    = source_alpha + ((destination->a * inverse + (channel_max / 2)) / channel_max);
    *destination                  = {output_rgb.r, output_rgb.g, output_rgb.b, output_alpha};
}

bool clip_line(gpu::vec2i_t dimensions, gpu::vec2i_t* from, gpu::vec2i_t* to)
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
} // namespace

void surface_s::copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) { *dst = src; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) { *dst = {src, src, src, src}; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) {
        glm::ivec4 tmp(*dst);
        tmp -= src.a;
        tmp  = glm::max(tmp, glm::ivec4{});
        *dst = tmp;
        *dst += src;
    };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) {
        glm::ivec4 tmp(*dst);
        tmp -= src;
        tmp  = glm::max(tmp, glm::ivec4{});
        *dst = tmp;
        *dst += src;
    };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::source_over(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, source_over_pixel);
}

void surface_s::source_over(gpu::recti_s rect, const rgba_pixel_t& color)
{
    raster_operation(
        ptr(), dimensions_, rect, [&color](gpu::vec2i_t, auto* destination) { source_over_pixel(color, destination); });
}

void surface_s::source_over_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color)
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    const double radius_x = bounds.size.x / 2.0;
    const double radius_y = bounds.size.y / 2.0;
    const double center_x = bounds.pos.x + radius_x;
    const double center_y = bounds.pos.y + radius_y;
    raster_operation(ptr(), dimensions_, bounds, [&](gpu::vec2i_t pos, auto* destination) {
        const double x = (pos.x + 0.5 - center_x) / radius_x;
        const double y = (pos.y + 0.5 - center_y) / radius_y;
        if ((x * x) + (y * y) <= 1.0) {
            source_over_pixel(color, destination);
        }
    });
}

gpu::vec2i_t surface_s::asset_dimensions(std::string_view resource_path)
{
    return load_asset(resource_path)->dimensions;
}

void surface_s::draw_asset(std::string_view resource_path, gpu::vec2i_t position)
{
    const auto asset = load_asset(resource_path);
    source_over(asset->pixels.data(),
                asset->dimensions,
                sizeof(rgba_pixel_t) * static_cast<size_t>(asset->dimensions.x),
                position);
}

void surface_s::fill(gpu::recti_s rect, const rgba_pixel_t& color)
{
    const auto clipped = clip_rect(rect, dimensions_);
    if (!clipped.has_value()) {
        return;
    }

    auto* row = ptr() + (static_cast<size_t>(clipped->begin.y) * static_cast<size_t>(dimensions_.x));
    for (int y = clipped->begin.y; y < clipped->end.y; ++y) {
        std::fill(row + clipped->begin.x, row + clipped->end.x, color);
        row += dimensions_.x;
    }
}

void surface_s::draw_rect(gpu::recti_s rect, const rgba_pixel_t& color, int thickness)
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

void surface_s::draw_line(gpu::vec2i_t from, gpu::vec2i_t to, const rgba_pixel_t& color, int thickness)
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

void surface_s::fill_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color)
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    const double radius_x = bounds.size.x / 2.0;
    const double radius_y = bounds.size.y / 2.0;
    const double center_x = bounds.pos.x + radius_x;
    const double center_y = bounds.pos.y + radius_y;
    raster_operation(ptr(), dimensions_, bounds, [&](gpu::vec2i_t pos, auto* dst) {
        const double x = (pos.x + 0.5 - center_x) / radius_x;
        const double y = (pos.y + 0.5 - center_y) / radius_y;
        if ((x * x) + (y * y) <= 1.0) {
            *dst = color;
        }
    });
}

void surface_s::draw_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color, int thickness)
{
    if (thickness <= 0 || bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }

    const double radius_x       = bounds.size.x / 2.0;
    const double radius_y       = bounds.size.y / 2.0;
    const double inner_radius_x = std::max(0.0, radius_x - thickness);
    const double inner_radius_y = std::max(0.0, radius_y - thickness);
    const double center_x       = bounds.pos.x + radius_x;
    const double center_y       = bounds.pos.y + radius_y;

    raster_operation(ptr(), dimensions_, bounds, [&](gpu::vec2i_t pos, auto* dst) {
        const double px    = pos.x + 0.5 - center_x;
        const double py    = pos.y + 0.5 - center_y;
        const double outer = ((px * px) / (radius_x * radius_x)) + ((py * py) / (radius_y * radius_y));
        const double inner =
            inner_radius_x > 0.0 && inner_radius_y > 0.0
                ? ((px * px) / (inner_radius_x * inner_radius_x)) + ((py * py) / (inner_radius_y * inner_radius_y))
                : std::numeric_limits<double>::infinity();
        if (outer <= 1.0 && inner >= 1.0) {
            *dst = color;
        }
    });
}

void surface_s::fill_circle(gpu::vec2i_t center, int radius, const rgba_pixel_t& color)
{
    if (radius <= 0) {
        return;
    }
    fill_ellipse(make_rect(center - gpu::vec2i_t{radius}, gpu::vec2i_t{radius * 2}), color);
}

void surface_s::draw_circle(gpu::vec2i_t center, int radius, const rgba_pixel_t& color, int thickness)
{
    if (radius <= 0) {
        return;
    }
    draw_ellipse(make_rect(center - gpu::vec2i_t{radius}, gpu::vec2i_t{radius * 2}), color, thickness);
}

void surface_s::horizontal_gradient(gpu::recti_s rect, const rgba_pixel_t& left, const rgba_pixel_t& right)
{
    raster_operation(ptr(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        *dst = interpolate(left, right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
    });
}

void surface_s::vertical_gradient(gpu::recti_s rect, const rgba_pixel_t& top, const rgba_pixel_t& bottom)
{
    raster_operation(ptr(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        *dst = interpolate(top, bottom, pos.y - rect.pos.y, std::max(rect.size.y - 1, 1));
    });
}

void surface_s::bilinear_gradient(gpu::recti_s        rect,
                                  const rgba_pixel_t& top_left,
                                  const rgba_pixel_t& top_right,
                                  const rgba_pixel_t& bottom_left,
                                  const rgba_pixel_t& bottom_right)
{
    raster_operation(ptr(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        const auto top    = interpolate(top_left, top_right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
        const auto bottom = interpolate(bottom_left, bottom_right, pos.x - rect.pos.x, std::max(rect.size.x - 1, 1));
        *dst              = interpolate(top, bottom, pos.y - rect.pos.y, std::max(rect.size.y - 1, 1));
    });
}

void surface_s::checkerboard(gpu::recti_s        rect,
                             gpu::vec2i_t        cell_size,
                             const rgba_pixel_t& first,
                             const rgba_pixel_t& second)
{
    if (cell_size.x <= 0 || cell_size.y <= 0) {
        throw std::invalid_argument("checkerboard cell size must be positive");
    }
    raster_operation(ptr(), dimensions_, rect, [&](gpu::vec2i_t pos, auto* dst) {
        const int cell_x = (pos.x - rect.pos.x) / cell_size.x;
        const int cell_y = (pos.y - rect.pos.y) / cell_size.y;
        *dst             = ((cell_x + cell_y) & 1) == 0 ? first : second;
    });
}

void surface_s::draw_grid(gpu::recti_s rect, gpu::vec2i_t spacing, const rgba_pixel_t& color, int thickness)
{
    if (spacing.x <= 0 || spacing.y <= 0) {
        throw std::invalid_argument("grid spacing must be positive");
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
