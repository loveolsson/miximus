#include "test_pattern.hpp"

#include "gpu/types.hpp"
#include "render/image_asset/image_asset.hpp"
#include "render/surface/surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>

namespace miximus::render {
namespace {
using pixel_t = surface_s::rgba_pixel_t;

constexpr pixel_t BLACK{0, 0, 0, 255};
constexpr pixel_t WHITE{255, 255, 255, 255};
// Logo border #2C86B0 converted from its Rec.709-encoded PNG value to linear surface bytes.
constexpr pixel_t    LOGO_BORDER{11, 73, 122, 191};
constexpr std::array LOGO_PATHS{
    std::string_view{"images/miximus_128x128.png"},
    std::string_view{"images/miximus_64x64.png"},
    std::string_view{"images/miximus_32x32.png"},
};

uint8_t rec709_to_linear(double encoded)
{
    encoded             = std::clamp(encoded, 0.0, 1.0);
    const double linear = encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
    return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 255.0));
}

pixel_t video_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    constexpr double black = 16.0;
    constexpr double range = 219.0;
    return {
        rec709_to_linear((red - black) / range),
        rec709_to_linear((green - black) / range),
        rec709_to_linear((blue - black) / range),
        255,
    };
}

gpu::recti_s full_rect(const surface_s& surface) { return {.pos = {}, .size = surface.dimensions()}; }

gpu::recti_s make_rect(gpu::vec2i_t position, gpu::vec2i_t size) { return {.pos = position, .size = size}; }

gpu::recti_s vertical_slice(gpu::vec2i_t dimensions, int index, int count, int top, int bottom)
{
    const int left  = static_cast<int>((static_cast<int64_t>(dimensions.x) * index) / count);
    const int right = static_cast<int>((static_cast<int64_t>(dimensions.x) * (index + 1)) / count);
    return make_rect({left, top}, {right - left, bottom - top});
}

void draw_bars(surface_s* surface, std::span<const pixel_t> colors, int top, int bottom)
{
    for (size_t i = 0; i < colors.size(); ++i) {
        surface->fill(
            vertical_slice(surface->dimensions(), static_cast<int>(i), static_cast<int>(colors.size()), top, bottom),
            colors[i]);
    }
}

void draw_ebu_color_bars(surface_s* surface)
{
    const auto       low  = uint8_t{16};
    const auto       high = uint8_t{180};
    const std::array colors{
        video_rgb(high, high, high),
        video_rgb(high, high, low),
        video_rgb(low, high, high),
        video_rgb(low, high, low),
        video_rgb(high, low, high),
        video_rgb(high, low, low),
        video_rgb(low, low, high),
        video_rgb(low, low, low),
    };
    draw_bars(surface, colors, 0, surface->dimensions().y);
}

void draw_smpte_color_bars(surface_s* surface)
{
    const auto dimensions = surface->dimensions();
    const auto low        = uint8_t{16};
    const auto high       = uint8_t{180};
    const int  upper_end  = dimensions.y * 2 / 3;
    const int  middle_end = dimensions.y * 3 / 4;

    const std::array upper{
        video_rgb(high, high, high),
        video_rgb(high, high, low),
        video_rgb(low, high, high),
        video_rgb(low, high, low),
        video_rgb(high, low, high),
        video_rgb(high, low, low),
        video_rgb(low, low, high),
    };
    const std::array middle{
        video_rgb(low, low, high),
        video_rgb(low, low, low),
        video_rgb(high, low, high),
        video_rgb(low, low, low),
        video_rgb(low, high, high),
        video_rgb(low, low, low),
        video_rgb(high, high, high),
    };
    draw_bars(surface, upper, 0, upper_end);
    draw_bars(surface, middle, upper_end, middle_end);

    const int first_end  = dimensions.x * 5 / 21;
    const int second_end = dimensions.x * 10 / 21;
    const int third_end  = dimensions.x * 15 / 21;
    surface->fill(make_rect({0, middle_end}, {first_end, dimensions.y - middle_end}), video_rgb(16, 61, 78));
    surface->fill(make_rect({first_end, middle_end}, {second_end - first_end, dimensions.y - middle_end}),
                  video_rgb(235, 235, 235));
    surface->fill(make_rect({second_end, middle_end}, {third_end - second_end, dimensions.y - middle_end}),
                  video_rgb(50, 16, 75));

    const std::array pluge{
        video_rgb(16, 16, 16),
        video_rgb(7, 7, 7),
        video_rgb(16, 16, 16),
        video_rgb(25, 25, 25),
        video_rgb(16, 16, 16),
        video_rgb(16, 16, 16),
    };
    const int pluge_width = dimensions.x - third_end;
    for (size_t i = 0; i < pluge.size(); ++i) {
        const int left  = third_end + static_cast<int>((static_cast<int64_t>(pluge_width) * i) / pluge.size());
        const int right = third_end + static_cast<int>((static_cast<int64_t>(pluge_width) * (i + 1)) / pluge.size());
        surface->fill(make_rect({left, middle_end}, {right - left, dimensions.y - middle_end}), pluge.at(i));
    }
}

void draw_grayscale_ramp(surface_s* surface)
{
    const auto rect = full_rect(*surface);
    for (int x = 0; x < rect.size.x; ++x) {
        const double encoded = rect.size.x > 1 ? static_cast<double>(x) / (rect.size.x - 1) : 0.0;
        const auto   value   = rec709_to_linear(encoded);
        surface->fill(make_rect({x, 0}, {1, rect.size.y}), pixel_t{value, value, value, 255});
    }

    constexpr int steps = 11;
    const int     strip = std::max(1, rect.size.y / 8);
    for (int i = 0; i < steps; ++i) {
        const auto value = rec709_to_linear(static_cast<double>(i) / (steps - 1));
        surface->fill(vertical_slice(rect.size, i, steps, rect.size.y - strip, rect.size.y),
                      pixel_t{value, value, value, 255});
    }
}

void draw_crosshatch(surface_s* surface)
{
    const auto dimensions = surface->dimensions();
    const auto rect       = full_rect(*surface);
    const int  minor      = std::max(8, std::min(dimensions.x, dimensions.y) / 16);
    const int  major      = minor * 4;
    const int  thickness  = std::max(1, std::min(dimensions.x, dimensions.y) / 720);

    surface->clear(video_rgb(16, 16, 16));
    surface->draw_grid(rect, {minor, minor}, video_rgb(48, 48, 48), thickness);
    surface->draw_grid(rect, {major, major}, video_rgb(180, 180, 180), thickness);
    surface->draw_rect(rect, video_rgb(235, 235, 235), thickness);

    const gpu::vec2i_t center = dimensions / 2;
    surface->draw_line({center.x, 0}, {center.x, dimensions.y - 1}, video_rgb(235, 235, 235), thickness);
    surface->draw_line({0, center.y}, {dimensions.x - 1, center.y}, video_rgb(235, 235, 235), thickness);
    surface->draw_circle(center, std::min(dimensions.x, dimensions.y) / 4, video_rgb(235, 235, 235), thickness);
}

void draw_checkerboard(surface_s* surface)
{
    const auto dimensions = surface->dimensions();
    const int  cell       = std::max(1, std::min(dimensions.x, dimensions.y) / 16);
    surface->checkerboard(full_rect(*surface), {cell, cell}, BLACK, WHITE);
}

void draw_multiburst(surface_s* surface)
{
    const auto dimensions = surface->dimensions();
    surface->clear(BLACK);
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        return;
    }

    constexpr int bands = 6;
    for (int band = 0; band < bands; ++band) {
        const int left   = dimensions.x * band / bands;
        const int right  = dimensions.x * (band + 1) / bands;
        const int width  = std::max(1, right - left);
        const int cycles = 1 << band;
        for (int x = left; x < right; ++x) {
            const double phase = static_cast<double>(x - left) / width * cycles * 2.0 * std::numbers::pi;
            const auto   value = rec709_to_linear((std::sin(phase) * 0.5) + 0.5);
            surface->fill(make_rect({x, 0}, {1, dimensions.y}), pixel_t{value, value, value, 255});
        }
    }
}

void draw_zone_plate(surface_s* surface)
{
    const auto dimensions = surface->dimensions();
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        return;
    }

    const double scale_x = 2.0 / dimensions.x;
    const double scale_y = 2.0 / dimensions.y;
    auto*        pixels  = surface->ptr();
    for (int y = 0; y < dimensions.y; ++y) {
        const double ny = ((y + 0.5) * scale_y) - 1.0;
        for (int x = 0; x < dimensions.x; ++x) {
            const double nx    = ((x + 0.5) * scale_x) - 1.0;
            const double phase = (nx * nx + ny * ny) * std::min(dimensions.x, dimensions.y) * std::numbers::pi;
            const auto   value = rec709_to_linear((std::cos(phase) * 0.5) + 0.5);
            pixels[(static_cast<size_t>(y) * static_cast<size_t>(dimensions.x)) + static_cast<size_t>(x)] = {
                value, value, value, 255};
        }
    }
}
} // namespace

void render_test_pattern(surface_s& surface, test_pattern_e pattern)
{
    switch (pattern) {
        case test_pattern_e::smpte_color_bars:
            draw_smpte_color_bars(&surface);
            break;
        case test_pattern_e::ebu_color_bars:
            draw_ebu_color_bars(&surface);
            break;
        case test_pattern_e::black_field:
            surface.clear(video_rgb(16, 16, 16));
            break;
        case test_pattern_e::white_field:
            surface.clear(video_rgb(235, 235, 235));
            break;
        case test_pattern_e::red_field:
            surface.clear(video_rgb(235, 16, 16));
            break;
        case test_pattern_e::green_field:
            surface.clear(video_rgb(16, 235, 16));
            break;
        case test_pattern_e::blue_field:
            surface.clear(video_rgb(16, 16, 235));
            break;
        case test_pattern_e::grayscale_ramp:
            draw_grayscale_ramp(&surface);
            break;
        case test_pattern_e::crosshatch:
            draw_crosshatch(&surface);
            break;
        case test_pattern_e::checkerboard:
            draw_checkerboard(&surface);
            break;
        case test_pattern_e::multiburst:
            draw_multiburst(&surface);
            break;
        case test_pattern_e::zone_plate:
            draw_zone_plate(&surface);
            break;
    }
}

void render_test_pattern_logo(surface_s& surface)
{
    const auto maximum_size = surface.dimensions() / 5;
    for (const auto resource_path : LOGO_PATHS) {
        const auto logo            = load_image_asset(resource_path);
        const auto logo_dimensions = image_asset_dimensions(*logo);
        if (logo_dimensions.x > maximum_size.x || logo_dimensions.y > maximum_size.y) {
            continue;
        }

        const auto logo_position   = (surface.dimensions() - logo_dimensions) / 2;
        int        circle_diameter = static_cast<int>(std::ceil(std::hypot(logo_dimensions.x, logo_dimensions.y)));
        if ((circle_diameter - logo_dimensions.x) % 2 != 0) {
            ++circle_diameter;
        }

        const gpu::vec2i_t circle_dimensions{circle_diameter};
        const auto         circle_position = logo_position - ((circle_dimensions - logo_dimensions) / 2);
        surface.source_over_ellipse(make_rect(circle_position, circle_dimensions), LOGO_BORDER);
        draw_image_asset(surface, *logo, logo_position);
        return;
    }
}

} // namespace miximus::render
