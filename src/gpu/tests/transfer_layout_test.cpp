#include "gpu/transfer/detail/transfer_layout.hpp"

#include <gtest/gtest.h>

namespace miximus::gpu::transfer::detail { namespace {

host_frame_layout_s make_layout(host_pixel_format_e format)
{
    return {
        .image_dimensions  = {1920, 1080},
        .pixel_format      = format,
        .row_stride_bytes  = size_t{1920}
           * 4,
        .buffer_size_bytes = size_t{1920}
           * 1080 * 4,
        .memory_access     = host_memory_access_e::read_only,
    };
}

TEST(TransferLayout, KeepsRgbaBytesRaw)
{
    const auto plan = make_texture_transfer_plan(make_layout(host_pixel_format_e::rgba_u8));

    EXPECT_EQ(plan.host_layout.image_dimensions, (vec2i_t{1920, 1080}));
    EXPECT_EQ(plan.packed_buffer_bytes, 0);
    EXPECT_EQ(plan.input_order, channel_order_e::rgba);
    EXPECT_EQ(plan.output_order, channel_order_e::rgba);
}

TEST(TransferLayout, DescribesBgraMappingWithoutChangingRawStorage)
{
    const auto plan = make_texture_transfer_plan(make_layout(host_pixel_format_e::bgra_u8));

    EXPECT_EQ(plan.packed_buffer_bytes, 0);
    EXPECT_EQ(plan.input_order, channel_order_e::bgra);
    EXPECT_EQ(plan.output_order, channel_order_e::bgra);
}

TEST(TransferLayout, ForcesOpaqueAlphaForBgrxInput)
{
    const auto plan = make_texture_transfer_plan(make_layout(host_pixel_format_e::bgrx_u8));

    EXPECT_EQ(plan.input_order, channel_order_e::bgrx);
    EXPECT_EQ(plan.output_order, channel_order_e::rgba);
}

TEST(TransferLayout, DescribesArgbMappingWithoutChangingRawStorage)
{
    const auto plan = make_texture_transfer_plan(make_layout(host_pixel_format_e::argb_u8));

    EXPECT_EQ(plan.packed_buffer_bytes, 0);
    EXPECT_EQ(plan.input_order, channel_order_e::argb);
    EXPECT_EQ(plan.output_order, channel_order_e::argb);
}

TEST(TransferLayout, StoresV210AsRawWordsIncludingRowPadding)
{
    auto layout              = make_layout(host_pixel_format_e::v210);
    layout.row_stride_bytes  = 5120;
    layout.buffer_size_bytes = size_t{5120} * 1080;

    const auto plan = make_texture_transfer_plan(layout);

    EXPECT_EQ(plan.host_layout.image_dimensions, (vec2i_t{1920, 1080}));
    EXPECT_EQ(plan.packed_buffer_bytes, size_t{5120} * 1080);
}

TEST(TransferLayout, RejectsV210RowsTooShortForTheImage)
{
    auto layout              = make_layout(host_pixel_format_e::v210);
    layout.row_stride_bytes  = 5104;
    layout.buffer_size_bytes = size_t{5104} * 1080;

    EXPECT_THROW((void)make_texture_transfer_plan(layout), std::invalid_argument);
}

}} // namespace miximus::gpu::transfer::detail
