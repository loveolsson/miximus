#include "render/detail/color_lut.hpp"
#include "render/detail/rec709_quantizer.hpp"
#include "render/surface/surface.hpp"
#include "render/test_pattern/test_pattern.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace miximus;
using pixel_t = render::surface_s::pixel_t;

TEST(StridedImageView, AddressesPaddedRowsFromTheLogicalFirstRow)
{
    const std::array<uint32_t, 6> storage{1, 2, 0, 3, 4, 0};
    const auto                    view = render::strided_image_view_s<uint32_t>::from_rows(
        storage.data(), {2, 2}, 3 * static_cast<ptrdiff_t>(sizeof(uint32_t)));

    EXPECT_EQ(view.row(0)[0], 1);
    EXPECT_EQ(view.row(0)[1], 2);
    EXPECT_EQ(view.row(1)[0], 3);
    EXPECT_EQ(view.row(1)[1], 4);
}

TEST(StridedImageView, AddressesNegativeStridesFromTheLogicalFirstRow)
{
    const std::array<uint32_t, 6> storage{1, 2, 0, 3, 4, 0};
    const auto                    view = render::strided_image_view_s<uint32_t>::from_rows(
        storage.data() + 3, {2, 2}, -3 * static_cast<ptrdiff_t>(sizeof(uint32_t)));

    EXPECT_EQ(view.row(0)[0], 3);
    EXPECT_EQ(view.row(0)[1], 4);
    EXPECT_EQ(view.row(1)[0], 1);
    EXPECT_EQ(view.row(1)[1], 2);
}

TEST(StridedImageView, RejectsStridesThatMisalignRows)
{
    const std::array<uint32_t, 4> storage{};
    EXPECT_THROW((void)render::strided_image_view_s<uint32_t>::from_rows(storage.data(), {2, 2}, 9),
                 std::invalid_argument);
}

TEST(Surface, AcceptsStorageWithoutPreferredAlignment)
{
    constexpr gpu::vec2i_t dimensions{2, 2};
    constexpr size_t       byte_size = sizeof(pixel_t) * dimensions.x * dimensions.y;
    std::vector<std::byte> storage(byte_size + render::surface_s::PREFERRED_DATA_ALIGNMENT);

    size_t offset = 1;
    while ((reinterpret_cast<std::uintptr_t>(storage.data() + offset) % render::surface_s::PREFERRED_DATA_ALIGNMENT) ==
           0) {
        ++offset;
    }

    render::surface_s surface(dimensions, std::span{storage}.subspan(offset, byte_size));
    surface.clear({1, 2, 3, 4});

    EXPECT_EQ(surface.pixels().front(), (pixel_t{1, 2, 3, 4}));
}

TEST(Surface, ConvertsStraightRgbaToPremultipliedSourceOver)
{
    std::vector<pixel_t> destination(1);
    render::surface_s    surface({1, 1}, destination);
    const std::vector    source{
        render::straight_rgba_pixel_s{.r = 200, .g = 100, .b = 50, .a = 128}
    };

    surface.source_over(render::strided_image_view_s<render::straight_rgba_pixel_s>::packed(source, {1, 1}), {});

    EXPECT_EQ(destination.front(), (pixel_t{100, 50, 25, 128}));
}

TEST(Surface, CompositesPremultipliedPixels)
{
    std::vector destination{
        pixel_t{0, 0, 255, 255}
    };
    render::surface_s surface({1, 1}, destination);
    const std::vector source{
        render::straight_rgba_pixel_s{.r = 255, .g = 0, .b = 0, .a = 128}
    };

    surface.source_over(render::strided_image_view_s<render::straight_rgba_pixel_s>::packed(source, {1, 1}), {});

    EXPECT_EQ(destination.front(), (pixel_t{128, 0, 127, 255}));
}

TEST(Surface, ConvertsFreeTypeSrgbPremultipliedBgraToCanonicalPixels)
{
    std::vector<pixel_t> destination(1);
    render::surface_s    surface({1, 1}, destination);
    const std::vector    source{
        render::srgb_premultiplied_bgra_pixel_s{.b = 0, .g = 0, .r = 255, .a = 255}
    };

    surface.source_over(render::strided_image_view_s<render::srgb_premultiplied_bgra_pixel_s>::packed(source, {1, 1}),
                        {});

    EXPECT_EQ(destination.front(), (pixel_t{255, 0, 0, 255}));
}

TEST(Surface, CompileTimeColorLutsMatchStandardTransferFunctions)
{
    for (size_t i = 0; i < 256; ++i) {
        const double srgb_encoded = static_cast<double>(i) / 255.0;
        const double srgb_linear =
            srgb_encoded <= 0.04045 ? srgb_encoded / 12.92 : std::pow((srgb_encoded + 0.055) / 1.055, 2.4);
        EXPECT_EQ(render::detail::SRGB_TO_LINEAR_U8.at(i),
                  static_cast<uint8_t>(std::lround(std::clamp(srgb_linear, 0.0, 1.0) * 255.0)));

        const double rec709_encoded = std::clamp((static_cast<double>(i) - 16.0) / 219.0, 0.0, 1.0);
        const double rec709_linear =
            rec709_encoded < 0.081 ? rec709_encoded / 4.5 : std::pow((rec709_encoded + 0.099) / 1.099, 1.0 / 0.45);
        EXPECT_EQ(render::detail::VIDEO_REC709_TO_LINEAR_U8.at(i),
                  static_cast<uint8_t>(std::lround(std::clamp(rec709_linear, 0.0, 1.0) * 255.0)));
    }
}

TEST(Surface, UsesGrayscaleGlyphsAsCoverage)
{
    std::vector<pixel_t>                        destination(1);
    render::surface_s                           surface({1, 1}, destination);
    const std::vector<render::coverage_pixel_t> source{128};

    surface.source_over(render::strided_image_view_s<render::coverage_pixel_t>::packed(source, {1, 1}), {});

    EXPECT_EQ(destination.front(), (pixel_t{128, 128, 128, 128}));
}

TEST(Surface, InvalidDrawingGeometryIsANoOp)
{
    constexpr pixel_t original{4, 3, 2, 1};
    std::vector       destination(4, original);
    render::surface_s surface({2, 2}, destination);

    surface.fill(
        {
            .pos = {},
              .size = {-1, 1}
    },
        {255, 255, 255, 255});
    surface.checkerboard(
        {
            .pos = {},
              .size = {2, 2}
    },
        {0, 1},
        {},
        {});
    surface.draw_grid(
        {
            .pos = {},
              .size = {2, 2}
    },
        {1, 0},
        {255, 255, 255, 255});

    EXPECT_EQ(destination, std::vector(4, original));
}

TEST(Surface, Rec709CutoffsAgreeWithShaderFormulaWithinRoundingPrecision)
{
    const render::detail::rec709_quantizer_s quantize;
    int                                      maximum_error = 0;
    bool                                     monotonic     = true;
    uint8_t                                  previous      = 0;
    for (int i = 0; i <= 65536; ++i) {
        const float encoded  = static_cast<float>(i) / 65536.0F;
        const float linear   = encoded < 0.081F ? encoded / 4.5F : std::pow((encoded + 0.099F) / 1.099F, 1.0F / 0.45F);
        const auto  expected = static_cast<int>(std::lround(linear * 255.0F));
        const auto  value    = quantize(encoded);
        maximum_error        = std::max(maximum_error, std::abs(static_cast<int>(value) - expected));
        monotonic            = monotonic && value >= previous;
        previous             = value;
    }
    EXPECT_LE(maximum_error, 1);
    EXPECT_TRUE(monotonic);
    EXPECT_EQ(quantize(0.0F), 0);
    EXPECT_EQ(quantize(1.0F), 255);
}

TEST(Surface, ZonePlateMatchesContinuousReferenceWithinRoundingPrecision)
{
    constexpr gpu::vec2i_t dimensions{1920, 1080};
    std::vector<pixel_t>   pixels(static_cast<size_t>(dimensions.x) * dimensions.y);
    render::surface_s      surface(dimensions, pixels);
    render::render_test_pattern(surface, render::test_pattern_e::zone_plate);

    int  maximum_error    = 0;
    bool opaque_grayscale = true;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto   row      = i / dimensions.x;
        const double nx       = ((static_cast<double>(i % dimensions.x) + 0.5) * (2.0 / dimensions.x)) - 1.0;
        const double ny       = ((static_cast<double>(row) + 0.5) * (2.0 / dimensions.y)) - 1.0;
        const double phase    = (nx * nx + ny * ny) * std::min(dimensions.x, dimensions.y) * std::numbers::pi;
        const double encoded  = (std::cos(phase) * 0.5) + 0.5;
        const double linear   = encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
        const auto   expected = static_cast<int>(std::lround(linear * 255.0));
        const auto   pixel    = pixels[i];
        maximum_error         = std::max(maximum_error, std::abs(static_cast<int>(pixel.r) - expected));
        opaque_grayscale      = opaque_grayscale && pixel.r == pixel.g && pixel.g == pixel.b && pixel.a == 255;
    }
    EXPECT_LE(maximum_error, 1);
    EXPECT_TRUE(opaque_grayscale);
}

} // namespace
