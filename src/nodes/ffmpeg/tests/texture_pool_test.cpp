#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/ffmpeg/detail/texture_pool.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace miximus::nodes::ffmpeg::detail { namespace {

struct TexturePoolTest : testing::Test
{
    std::unique_ptr<gpu::device_s>               device;
    gpu::transfer::detail::memory_budget_s       budget{size_t{1} << 20};
    std::vector<std::shared_ptr<gpu::texture_s>> pool;
    size_t                                       reserved{};

    void SetUp() override
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::info);
        }
        device = std::make_unique<gpu::device_s>(gpu::device_options_s{.validation = true});
    }

    void add(gpu::vec2i_t dimensions = {64, 64})
    {
        const auto bytes = gpu::texture_s::estimate_storage_byte_size(dimensions, gpu::format_e::rgba_unorm16);
        ASSERT_TRUE(budget.reserve_memory(bytes));
        reserved += bytes;
        pool.push_back(std::make_shared<gpu::texture_s>(*device, dimensions, gpu::format_e::rgba_unorm16));
    }
};

TEST_F(TexturePoolTest, SoftwareFallbackReturnsUnusedReservations)
{
    for (int i = 0; i < 6; ++i) {
        add();
    }
    const auto used = reserved;
    ASSERT_GT(used, 0U);
    retire_unused_textures(pool, budget, reserved);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(reserved, 0U);
    EXPECT_EQ(budget.usage(), 0U);
    EXPECT_TRUE(budget.reserve_memory(used));
    budget.release_memory(used);
}

TEST_F(TexturePoolTest, RetainsDisplayedAndRecordedTexturesUntilSafe)
{
    add();
    add();
    add();
    auto displayed = pool.front();
    auto record    = device->try_record();
    ASSERT_NE(record, nullptr);
    record->clear(*pool.at(1));
    const auto per_texture = reserved / 3;

    retire_unused_textures(pool, budget, reserved);
    EXPECT_EQ(pool.size(), 2U);
    EXPECT_EQ(budget.usage(), per_texture * 2);

    displayed.reset();
    retire_unused_textures(pool, budget, reserved);
    EXPECT_EQ(pool.size(), 1U);
    EXPECT_EQ(budget.usage(), per_texture);

    const auto completion = record->submit();
    record.reset();
    ASSERT_EQ(completion.wait(std::chrono::seconds(10)), gpu::wait_result_e::ready);
    device->collect();
    retire_unused_textures(pool, budget, reserved);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(reserved, 0U);
    EXPECT_EQ(budget.usage(), 0U);
    EXPECT_EQ(device->validation_errors(), 0U);
}

TEST_F(TexturePoolTest, ResolutionRetirementPreservesReusableTexturesAndOtherReservations)
{
    add({32, 32});
    add({64, 64});
    ASSERT_TRUE(budget.reserve_memory(128));
    reserved += 128;
    const auto retained_bytes = gpu::texture_s::estimate_storage_byte_size({64, 64}, gpu::format_e::rgba_unorm16);

    retire_unused_textures(pool, budget, reserved, gpu::vec2i_t{64, 64});
    ASSERT_EQ(pool.size(), 1U);
    EXPECT_EQ(reserved, retained_bytes + 128);
    EXPECT_EQ(budget.usage(), reserved);

    retire_unused_textures(pool, budget, reserved);
    EXPECT_EQ(reserved, 128U);
    EXPECT_EQ(budget.usage(), 128U);
    budget.release_memory(128);
}

}} // namespace miximus::nodes::ffmpeg::detail
