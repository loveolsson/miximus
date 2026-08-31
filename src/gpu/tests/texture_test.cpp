#include "gpu/texture.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

namespace miximus::gpu { namespace {

TEST(Texture, FullMipChainUsesEveryLegalLevel)
{
    using enum texture_s::sampling_e;
    using enum texture_s::storage_format_e;

    EXPECT_EQ(texture_s::mip_map_level_count({1, 1}, rgba_unorm8, mipmapped_linear), 1);
    EXPECT_EQ(texture_s::mip_map_level_count({2, 1}, rgba_unorm8, mipmapped_linear), 2);
    EXPECT_EQ(texture_s::mip_map_level_count({4, 3}, rgba_unorm8, mipmapped_linear), 3);
    EXPECT_EQ(texture_s::mip_map_level_count({1920, 1080}, rgba_unorm8, mipmapped_linear), 11);
}

TEST(Texture, LinearAndIntegerTexturesUseOnlyTheBaseLevel)
{
    using enum texture_s::sampling_e;
    using enum texture_s::storage_format_e;

    EXPECT_EQ(texture_s::mip_map_level_count({1920, 1080}, rgba_unorm8, linear), 1);
    EXPECT_EQ(texture_s::mip_map_level_count({1920, 1080}, rgba_unorm8, nearest), 1);
    EXPECT_EQ(texture_s::mip_map_level_count({1920, 1080}, r32_uint, mipmapped_linear), 1);
}

TEST(Texture, MipLevelCalculationRejectsInvalidDimensions)
{
    EXPECT_THROW(texture_s::mip_map_level_count(
                     {0, 1080}, texture_s::storage_format_e::rgba_unorm8, texture_s::sampling_e::mipmapped_linear),
                 std::invalid_argument);
}

TEST(Texture, StorageEstimateMatchesTheSelectedSamplingPolicy)
{
    using enum texture_s::sampling_e;
    using enum texture_s::storage_format_e;

    EXPECT_EQ(texture_s::estimate_storage_byte_size({4, 4}, rgba_unorm8, linear), 64);
    EXPECT_EQ(texture_s::estimate_storage_byte_size({4, 4}, rgba_unorm8, mipmapped_linear), 84);
    EXPECT_EQ(texture_s::estimate_storage_byte_size({1, 1}, rgba_unorm8, mipmapped_linear), 4);
}

}} // namespace miximus::gpu
