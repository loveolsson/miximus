#include "gpu/geometry.hpp"

#include <gtest/gtest.h>

namespace miximus::gpu { namespace {

TEST(Geometry, ScreenScaleUsesTheCompleteOutputTarget)
{
    const auto draw = calculate_texture_draw({}, {1920, 1080}, {2000, 500}, fill_mode_e::scale);

    EXPECT_EQ(draw.destination, rect_s{});
    EXPECT_EQ(draw.source, rect_s{});
}

TEST(Geometry, ScreenContainLetterboxesOnTheRenderTarget)
{
    const auto draw = calculate_texture_draw({}, {1920, 1080}, {2000, 500}, fill_mode_e::contain);

    EXPECT_NEAR(draw.destination.pos.x, 5.0 / 18.0, 1e-12);
    EXPECT_DOUBLE_EQ(draw.destination.pos.y, 0.0);
    EXPECT_NEAR(draw.destination.size.x, 4.0 / 9.0, 1e-12);
    EXPECT_DOUBLE_EQ(draw.destination.size.y, 1.0);
    EXPECT_EQ(draw.source, rect_s{});
}

TEST(Geometry, ScreenFillCropsTheSourceOnTheRenderTarget)
{
    const auto draw = calculate_texture_draw({}, {1920, 1080}, {2000, 500}, fill_mode_e::fill);

    EXPECT_EQ(draw.destination, rect_s{});
    EXPECT_DOUBLE_EQ(draw.source.pos.x, 0.0);
    EXPECT_NEAR(draw.source.pos.y, 5.0 / 18.0, 1e-12);
    EXPECT_DOUBLE_EQ(draw.source.size.x, 1.0);
    EXPECT_NEAR(draw.source.size.y, 4.0 / 9.0, 1e-12);
}

}} // namespace miximus::gpu
