#include "render/surface/surface.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <span>
#include <vector>

namespace {
using namespace miximus;
using pixel_t = render::surface_s::pixel_t;

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
        render::straight_rgba_pixel_s{200, 100, 50, 128}
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
        render::straight_rgba_pixel_s{255, 0, 0, 128}
    };

    surface.source_over(render::strided_image_view_s<render::straight_rgba_pixel_s>::packed(source, {1, 1}), {});

    EXPECT_EQ(destination.front(), (pixel_t{128, 0, 127, 255}));
}

TEST(Surface, ConvertsFreeTypeSrgbPremultipliedBgraToCanonicalPixels)
{
    std::vector<pixel_t> destination(1);
    render::surface_s    surface({1, 1}, destination);
    const std::vector    source{
        render::srgb_premultiplied_bgra_pixel_s{0, 0, 255, 255}
    };

    surface.source_over(render::strided_image_view_s<render::srgb_premultiplied_bgra_pixel_s>::packed(source, {1, 1}),
                        {});

    EXPECT_EQ(destination.front(), (pixel_t{255, 0, 0, 255}));
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

} // namespace
