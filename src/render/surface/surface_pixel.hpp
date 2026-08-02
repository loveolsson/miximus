#pragma once
#include <cstdint>
#include <type_traits>

namespace miximus::render {

// Canonical surface storage. RGB is linear and premultiplied by alpha.
struct premultiplied_rgba_pixel_s
{
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};

    auto operator<=>(const premultiplied_rgba_pixel_s&) const = default;
};

// Straight-alpha linear RGBA, accepted by source-over operations.
struct straight_rgba_pixel_s
{
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};

    auto operator<=>(const straight_rgba_pixel_s&) const = default;
};

// FreeType color glyph storage: sRGB, premultiplied alpha, BGRA byte order.
struct srgb_premultiplied_bgra_pixel_s
{
    uint8_t b{};
    uint8_t g{};
    uint8_t r{};
    uint8_t a{};

    auto operator<=>(const srgb_premultiplied_bgra_pixel_s&) const = default;
};

using coverage_pixel_t = uint8_t;

static_assert(sizeof(premultiplied_rgba_pixel_s) == 4);
static_assert(alignof(premultiplied_rgba_pixel_s) == alignof(uint8_t));
static_assert(std::is_trivially_copyable_v<premultiplied_rgba_pixel_s>);
static_assert(sizeof(straight_rgba_pixel_s) == 4);
static_assert(std::is_trivially_copyable_v<straight_rgba_pixel_s>);
static_assert(sizeof(srgb_premultiplied_bgra_pixel_s) == 4);
static_assert(std::is_trivially_copyable_v<srgb_premultiplied_bgra_pixel_s>);

} // namespace miximus::render
