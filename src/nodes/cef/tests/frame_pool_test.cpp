#include "logger/logger.hpp"
#include "nodes/cef/detail/frame_pool.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace miximus::nodes::cef::detail { namespace {

using namespace std::chrono_literals;

class frame_pool_test : public testing::Test
{
  protected:
    static inline std::unique_ptr<gpu::device_s> device;
    static constexpr size_t                      budget = 1024 * 1024;

    static void SetUpTestSuite()
    {
        logger::init_loggers(spdlog::level::warn);
        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        device             = std::make_unique<gpu::device_s>(options);
    }

    static void TearDownTestSuite()
    {
        EXPECT_EQ(device->validation_errors(), 0U);
        device.reset();
    }
};

TEST_F(frame_pool_test, RetainedFramesBoundCapacity)
{
    frame_pool_s pool(*device, {32, 32}, 2, budget);
    auto         first  = pool.try_acquire();
    auto         second = pool.try_acquire();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(pool.try_acquire());

    std::shared_ptr<const frame_pool_s::frame_s> selected = first;
    first.reset();
    EXPECT_FALSE(pool.try_acquire());
    selected.reset();
    EXPECT_TRUE(pool.try_acquire());
}

TEST_F(frame_pool_test, UnsubmittedConsumerPreventsReuseAfterLeaseRelease)
{
    frame_pool_s pool(*device, {32, 32}, 1, budget);
    auto         frame = pool.try_acquire();
    ASSERT_TRUE(frame);
    gpu::texture_s output(*device, {32, 32}, gpu::format_e::rgba_unorm16);
    auto           context = device->create_recording_context();
    auto           record  = context.try_record();
    ASSERT_TRUE(record);
    record->draw(frame->texture(), output);
    frame.reset();
    EXPECT_FALSE(pool.try_acquire());

    // Abandoning a recording must release its tentative image use as well.
    record.reset();
    EXPECT_TRUE(pool.try_acquire());
}

TEST_F(frame_pool_test, IndependentProducerCompletesBeforeConsumerUsesFrame)
{
    frame_pool_s   pool(*device, {32, 32}, 1, budget);
    auto           producer_context = device->create_recording_context(1);
    auto           consumer_context = device->create_recording_context(1);
    gpu::texture_s source(*device, {32, 32}, gpu::format_e::rgba_unorm16);
    gpu::texture_s output(*device, {32, 32}, gpu::format_e::rgba_unorm16);

    auto setup = consumer_context.try_record();
    ASSERT_TRUE(setup);
    setup->clear(source, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto source_ready = setup->submit();
    setup.reset();

    // This producer stands in for the callback side. Only GPU operations move
    // pixels; no readback or CPU paint implementation is used by this test.
    std::promise<std::shared_ptr<const frame_pool_s::frame_s>> published;
    auto                                                       result = published.get_future();
    std::jthread                                               producer([&] {
        try {
            auto frame = pool.try_acquire();
            if (!frame) {
                throw std::runtime_error("Producer could not acquire frame");
            }
            auto record = producer_context.try_record();
            if (!record) {
                throw std::runtime_error("Producer could not acquire recording");
            }
            record->wait_for(source_ready);
            record->draw(source, frame->texture(), {.compositing = gpu::compositing_e::replace});
            const auto ready = record->submit();
            if (ready.wait(5s) != gpu::wait_result_e::ready) {
                throw std::runtime_error("Producer GPU work did not finish");
            }
            published.set_value(std::move(frame));
        } catch (...) {
            published.set_exception(std::current_exception());
        }
    });

    auto selected = result.get();
    producer.join();
    ASSERT_TRUE(selected);
    EXPECT_FALSE(pool.try_acquire());
    auto consumer = consumer_context.try_record();
    ASSERT_TRUE(consumer);
    consumer->draw(selected->texture(), output);
    selected.reset();
    EXPECT_FALSE(pool.try_acquire());
    const auto consumed = consumer->submit();
    consumer.reset();
    ASSERT_EQ(consumed.wait(5s), gpu::wait_result_e::ready);
    EXPECT_TRUE(pool.try_acquire());
}

TEST_F(frame_pool_test, OldGenerationSurvivesPoolReplacement)
{
    auto pool      = std::make_unique<frame_pool_s>(*device, gpu::vec2i_t{32, 32}, 1, budget);
    auto old_frame = pool->try_acquire();
    ASSERT_TRUE(old_frame);
    pool           = std::make_unique<frame_pool_s>(*device, gpu::vec2i_t{64, 64}, 1, budget);
    auto new_frame = pool->try_acquire();
    ASSERT_TRUE(new_frame);
    EXPECT_EQ(old_frame->texture().dimensions(), (gpu::vec2i_t{32, 32}));
    EXPECT_EQ(new_frame->texture().dimensions(), (gpu::vec2i_t{64, 64}));
    pool.reset();
    EXPECT_TRUE(old_frame->texture());
    EXPECT_TRUE(new_frame->texture());
}

TEST_F(frame_pool_test, RejectsInvalidOrOverBudgetGenerations)
{
    EXPECT_THROW((frame_pool_s(*device, {32, 32}, 0, budget)), std::invalid_argument);
    EXPECT_THROW((frame_pool_s(*device, {0, 32}, 1, budget)), std::invalid_argument);
    EXPECT_THROW((frame_pool_s(*device, {32, 32}, 2, 32 * 32 * 8)), std::invalid_argument);
}

}} // namespace miximus::nodes::cef::detail
