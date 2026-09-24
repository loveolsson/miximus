#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/detail/dma_buf_export.hpp"
#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "logger/logger.hpp"
#include "nodes/cef/detail/media_input_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace miximus::nodes::cef::detail { namespace {
using namespace std::chrono_literals;

TEST(media_input_vulkan, EightInputsReuseExportSlotsAfterCompletedGpuCopies)
{
    if (!spdlog::get("gpu")) {
        logger::init_loggers(spdlog::level::warn);
    }
    gpu::device_options_s options;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* uuid = std::getenv("MIXIMUS_VULKAN_DEVICE")) {
        options.device_uuid = uuid;
    }
    options.external_image_import = true;
    gpu::device_s producer(options);
    gpu::device_s consumer(options);
    ASSERT_TRUE(producer.external_image_import_support().enabled);
    auto                            producer_context = producer.create_recording_context(1);
    auto                            consumer_context = consumer.create_recording_context(1);
    constexpr gpu::extent_s         extent{.width = 640, .height = 360};
    auto                            source      = producer.create_texture(extent);
    auto                            destination = consumer.create_texture(extent);
    gpu::detail::color_comparison_s compare(destination, MIXIMUS_CEF_COMPARE_SHADER);
    auto                            counters = consumer.create_buffer(8, gpu::host_access_e::read_write);
    gpu::draw_s                     conversion;
    conversion.compositing = gpu::compositing_e::replace;

    for (size_t depth : {1U, 2U, 3U}) {
        media_input_pool_s                                          pool(depth);
        std::vector<std::unique_ptr<gpu::detail::dma_buf_export_s>> exports;
        for (size_t index = 0; index < depth * media_input_pool_s::INPUT_COUNT; ++index) {
            exports.push_back(std::make_unique<gpu::detail::dma_buf_export_s>(producer, extent));
            EXPECT_GT(exports.back()->allocation_bytes(), 0U);
        }
        // Fill all slots before consuming, then change every slot's pixels on
        // every round. Static textures cannot catch premature reuse/stale reads.
        for (size_t round = 0; round < 4; ++round) {
            {
                auto abandoned = producer_context.try_record();
                ASSERT_TRUE(abandoned);
                abandoned->clear(source, {1, 1, 1, 1});
                exports.front()->copy(*abandoned, source, conversion);
                // No native submission: first-use/reacquire ownership must not
                // change, and the next recording must still work.
            }
            std::vector<media_input_pool_s::ticket_s> tickets;
            std::vector<std::array<float, 4>>         colors;
            for (size_t input = 0; input < media_input_pool_s::INPUT_COUNT; ++input) {
                for (size_t slot = 0; slot < depth; ++slot) {
                    const auto ticket = pool.acquire(input);
                    ASSERT_TRUE(ticket);
                    tickets.push_back(*ticket);
                    const std::array<float, 4> color{
                        float((round + slot) % 4) / 4, float(input) / 8, float(round) / 4, 1};
                    colors.push_back(color);
                    auto& exported = *exports[input * depth + slot];
                    auto  record   = producer_context.try_record();
                    ASSERT_TRUE(record);
                    record->clear(source, color);
                    exported.copy(*record, source, conversion);
                    record->on_submitted([&pool, ticket = *ticket](gpu::completion_s /* completion */) {
                        EXPECT_TRUE(pool.publish(ticket));
                    });
                    ASSERT_EQ(record->submit().wait(5s), gpu::wait_result_e::ready);
                    ASSERT_TRUE(pool.producer_finished(*ticket));
                }
                EXPECT_FALSE(pool.acquire(input));
            }
            for (size_t index = 0; index < tickets.size(); ++index) {
                const auto ticket = tickets[index];
                ASSERT_TRUE(pool.begin_consume(ticket));
                auto& exported = *exports[ticket.input * depth + ticket.slot];
                auto  record   = consumer_context.try_record();
                ASSERT_TRUE(record);
                // Host waits are test orchestration, never CPU pixel transfer.
                // Producer completion above precedes any import/read of its FD.
                const auto completion =
                    gpu::detail::dma_buf_copy_s::submit(*record, exported.descriptor(), destination, conversion, 500ms);
                ASSERT_EQ(completion.wait(5s), gpu::wait_result_e::ready);
                record.reset();
                ASSERT_TRUE(pool.consumer_finished(ticket));
                std::ranges::fill(counters.writable_bytes(), std::byte{});
                record = consumer_context.try_record();
                ASSERT_TRUE(record);
                compare.record(*record, destination, counters, colors[index], 0.005F);
                ASSERT_EQ(record->submit().wait(5s), gpu::wait_result_e::ready);
                record.reset();
                std::array<uint32_t, 2> errors{};
                const auto              bytes = counters.readable_bytes();
                std::memcpy(errors.data(), bytes.data(), sizeof(errors));
                EXPECT_EQ(errors[0], 0U) << "depth=" << depth << " input=" << ticket.input << " round=" << round;
            }
        }
        for (size_t input = 0; input < media_input_pool_s::INPUT_COUNT; ++input) {
            EXPECT_EQ(pool.metrics(input).occupied, 0U);
            EXPECT_EQ(pool.metrics(input).high_water, depth);
        }
    }
    EXPECT_EQ(producer.validation_errors(), 0U);
    EXPECT_EQ(consumer.validation_errors(), 0U);
}

}} // namespace miximus::nodes::cef::detail
