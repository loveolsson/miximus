#include "gpu/color_transfer.hpp"
#include "gpu/drawing.hpp"
#include "gpu/video_frame.hpp"

#include <gtest/gtest.h>

namespace miximus::gpu { namespace {

TEST(VideoColor, Rec2020PrimariesMatchExistingBroadcastGamut)
{
    const auto actual   = video_gamut_to_rec709({
        .red = {0.708,  0.292 },
          .green = {0.170,  0.797 },
          .blue = {0.131,  0.046 },
          .white = {0.3127, 0.3290}
    });
    const auto expected = get_gamut_transfer_to_rec709(color_transfer_e::Rec2020);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            EXPECT_NEAR(actual[row][col], expected[row][col], 2e-6);
        }
    }
}

TEST(VideoColor, LimitedBlackAndWhiteMatchAtEverySupportedDepth)
{
    for (unsigned depth : {8U, 10U, 12U, 16U}) {
        const auto maximum    = static_cast<float>((1U << depth) - 1);
        const auto scale      = static_cast<float>(1U << (depth - 8));
        const auto conversion = get_color_transfer_from_yuv(color_transfer_e::Rec709, depth, false);
        const auto black =
            conversion.matrix * (vec3_t{16 * scale, 128 * scale, 128 * scale} / maximum - conversion.offset);
        const auto white =
            conversion.matrix * (vec3_t{235 * scale, 128 * scale, 128 * scale} / maximum - conversion.offset);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(black[i], 0, 1e-6);
            EXPECT_NEAR(white[i], 1, 1e-6);
        }
    }
}

TEST(VideoColor, FullRangeUsesExactChromaCenter)
{
    const auto conversion = get_color_transfer_from_yuv(color_transfer_e::Rec709, 8, true);
    const auto black      = conversion.matrix * (vec3_t{0, 128.0F / 255, 128.0F / 255} - conversion.offset);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(black[i], 0, 1e-6);
    }

    EXPECT_THROW(static_cast<void>(get_color_transfer_from_yuv(color_transfer_e::Rec709, 7)), std::invalid_argument);
    EXPECT_THROW(video_component_encoding(16, 17, 0, 0, false), std::invalid_argument);
}
}} // namespace miximus::gpu
