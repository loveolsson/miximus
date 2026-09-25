#include "nodes/cef/detail/media_input_pool.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace miximus::nodes::cef::detail { namespace {

media_input_pool_s::ticket_s acquire_ticket(media_input_pool_s& pool, size_t input)
{
    const auto ticket = pool.acquire(input);
    if (!ticket) {
        throw std::runtime_error("Expected a free media input slot");
    }

    return *ticket;
}

TEST(media_input_pool, ValidatesLimits)
{
    EXPECT_THROW(media_input_pool_s(0), std::invalid_argument);
    EXPECT_THROW(media_input_pool_s(9), std::invalid_argument);
    media_input_pool_s pool(1);
    EXPECT_THROW(pool.acquire(8), std::out_of_range);
    EXPECT_THROW(pool.invalidate(8), std::out_of_range);
    EXPECT_THROW(pool.metrics(8), std::out_of_range);
    EXPECT_FALSE(pool.publish({}));
    EXPECT_FALSE(pool.producer_finished({.input = 8}));
}

TEST(media_input_pool, AllEightInputsHaveIndependentCapacityAtEveryDepth)
{
    for (size_t depth = 1; depth <= media_input_pool_s::MAX_DEPTH; ++depth) {
        media_input_pool_s pool(depth);
        for (size_t input = 0; input < media_input_pool_s::INPUT_COUNT; ++input) {
            for (size_t slot = 0; slot < depth; ++slot) {
                const auto ticket = acquire_ticket(pool, input);
                EXPECT_EQ(ticket.input, input);
                EXPECT_EQ(ticket.slot, slot);
            }

            EXPECT_FALSE(pool.acquire(input));
            EXPECT_EQ(pool.metrics(input).occupied, depth);
            EXPECT_EQ(pool.metrics(input).capacity_drops, 1U);
        }
    }
}

TEST(media_input_pool, SubmissionAndReceiptCannotReleaseStorage)
{
    media_input_pool_s pool(1);
    const auto         ticket = acquire_ticket(pool, 0);
    EXPECT_FALSE(pool.begin_consume(ticket));
    EXPECT_FALSE(pool.producer_finished(ticket));
    ASSERT_TRUE(pool.publish(ticket));
    EXPECT_FALSE(pool.publish(ticket));
    EXPECT_FALSE(pool.abandon(ticket));
    EXPECT_FALSE(pool.begin_consume(ticket));
    EXPECT_FALSE(pool.consumer_finished(ticket));
    EXPECT_FALSE(pool.acquire(0));
    ASSERT_TRUE(pool.producer_finished(ticket));
    EXPECT_FALSE(pool.producer_finished(ticket));
    ASSERT_TRUE(pool.begin_consume(ticket));
    EXPECT_FALSE(pool.begin_consume(ticket));
    EXPECT_FALSE(pool.acquire(0));
    ASSERT_TRUE(pool.consumer_finished(ticket));
    EXPECT_TRUE(pool.acquire(0));
}

TEST(media_input_pool, ResizeAndNavigationRetainOutstandingGpuWork)
{
    media_input_pool_s pool(3);
    const auto         reserved = acquire_ticket(pool, 0);
    const auto         producer = acquire_ticket(pool, 0);
    const auto         consumer = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(producer));
    ASSERT_TRUE(pool.publish(consumer));
    ASSERT_TRUE(pool.producer_finished(consumer));
    ASSERT_TRUE(pool.begin_consume(consumer));
    pool.invalidate(0);
    pool.invalidate(0);
    EXPECT_FALSE(pool.acquire(0));
    EXPECT_FALSE(pool.begin_consume(producer));
    EXPECT_TRUE(pool.abandon(reserved));
    EXPECT_TRUE(pool.producer_finished(producer));
    EXPECT_EQ(pool.metrics(0).occupied, 1U);
    EXPECT_TRUE(pool.consumer_finished(consumer));
    const auto replacement = acquire_ticket(pool, 0);
    EXPECT_GT(replacement.generation, consumer.generation);
    EXPECT_GT(replacement.serial, consumer.serial);
    EXPECT_EQ(pool.metrics(0).retired, 3U);
}

TEST(media_input_pool, InvalidatedReservationCanStillReportRacingSubmission)
{
    media_input_pool_s pool(1);
    const auto         ticket = acquire_ticket(pool, 0);
    pool.invalidate(0);
    EXPECT_TRUE(pool.publish(ticket));
    EXPECT_FALSE(pool.abandon(ticket));
    EXPECT_FALSE(pool.acquire(0));
    EXPECT_TRUE(pool.producer_finished(ticket));
    EXPECT_FALSE(pool.begin_consume(ticket));
    EXPECT_TRUE(pool.acquire(0));
}

TEST(media_input_pool, OldAndDuplicateReleasesCannotRetireReusedSlot)
{
    media_input_pool_s pool(1);
    const auto         old = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(old));
    ASSERT_TRUE(pool.producer_finished(old));
    ASSERT_TRUE(pool.begin_consume(old));
    ASSERT_TRUE(pool.consumer_finished(old));
    const auto current = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(current));
    ASSERT_TRUE(pool.producer_finished(current));
    ASSERT_TRUE(pool.begin_consume(current));
    EXPECT_FALSE(pool.consumer_finished(old));
    EXPECT_FALSE(pool.cancel(old));
    EXPECT_FALSE(pool.abandon(old));
    EXPECT_FALSE(pool.producer_finished(old));
    EXPECT_EQ(pool.metrics(0).occupied, 1U);
    ASSERT_TRUE(pool.consumer_finished(current));
}

TEST(media_input_pool, CancellationWaitsForTheOwnerOfEachStage)
{
    media_input_pool_s pool(1);
    auto               ticket = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.cancel(ticket));
    EXPECT_FALSE(pool.acquire(0));
    ASSERT_TRUE(pool.abandon(ticket));
    ticket = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(ticket));
    ASSERT_TRUE(pool.cancel(ticket));
    EXPECT_FALSE(pool.acquire(0));
    ASSERT_TRUE(pool.producer_finished(ticket));
    ticket = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(ticket));
    ASSERT_TRUE(pool.producer_finished(ticket));
    ASSERT_TRUE(pool.cancel(ticket));
    EXPECT_EQ(pool.metrics(0).occupied, 0U);
    ticket = acquire_ticket(pool, 0);
    ASSERT_TRUE(pool.publish(ticket));
    ASSERT_TRUE(pool.producer_finished(ticket));
    ASSERT_TRUE(pool.begin_consume(ticket));
    ASSERT_TRUE(pool.cancel(ticket));
    EXPECT_FALSE(pool.acquire(0));
    ASSERT_TRUE(pool.consumer_finished(ticket));
}

TEST(media_input_pool, ConcurrentInputsAndNavigationStayBounded)
{
    media_input_pool_s                                        pool(1);
    std::array<std::jthread, media_input_pool_s::INPUT_COUNT> producers;
    for (size_t input = 0; input < producers.size(); ++input) {
        producers.at(input) = std::jthread([&pool, input] {
            for (size_t frame = 0; frame < 1000; ++frame) {
                const auto ticket = pool.acquire(input);
                ASSERT_TRUE(ticket);
                ASSERT_TRUE(pool.publish(*ticket));
                ASSERT_TRUE(pool.producer_finished(*ticket));
                if (pool.begin_consume(*ticket)) {
                    ASSERT_TRUE(pool.consumer_finished(*ticket));
                }
            }
        });
    }

    for (size_t frame = 0; frame < 1000; ++frame) {
        for (size_t input = 0; input < producers.size(); ++input) {
            pool.invalidate(input);
        }
    }

    for (auto& producer : producers) {
        producer.join();
    }

    for (size_t input = 0; input < producers.size(); ++input) {
        EXPECT_EQ(pool.metrics(input).occupied, 0U);
        EXPECT_EQ(pool.metrics(input).admitted, 1000U);
        EXPECT_EQ(pool.metrics(input).retired, 1000U);
        EXPECT_EQ(pool.metrics(input).high_water, 1U);
    }
}

}} // namespace miximus::nodes::cef::detail
