#include "media/source_clock.hpp"
#include "media/timed_source_queue.hpp"
#include "utils/flicks.hpp"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
using namespace miximus;

constexpr auto FRAME_DURATION = utils::flicks{11'771'760};

media::media_frame_id_s make_frame_id(uint64_t sequence, utils::flicks pts, uint64_t epoch = 1)
{
    return {
        .epoch    = epoch,
        .sequence = sequence,
        .pts      = pts,
        .duration = FRAME_DURATION,
    };
}

template <typename T>
T require_value(std::optional<T> value)
{
    if (!value.has_value()) {
        throw std::logic_error("test expected an optional value");
    }
    return std::move(value).value();
}

TEST(SourceClock, MapsArbitraryOriginsAndFiltersArrivalJitter)
{
    constexpr auto SOURCE_ORIGIN  = utils::to_flicks(37.0);
    constexpr auto PROGRAM_ORIGIN = utils::to_flicks(2.0);
    constexpr auto JITTER         = utils::to_flicks(0.004);
    constexpr auto ADJUSTMENT     = utils::to_flicks(0.001);

    media::source_clock_estimator_s clock({
        .phase_filter_divisor     = 4,
        .maximum_phase_adjustment = ADJUSTMENT,
        .discontinuity_threshold  = utils::to_flicks(0.5),
    });

    EXPECT_EQ(clock.observe(make_frame_id(1, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::source_clock_observation_e::initialized);
    EXPECT_EQ(require_value(clock.map(SOURCE_ORIGIN)), PROGRAM_ORIGIN);

    const auto source_pts = SOURCE_ORIGIN + FRAME_DURATION;
    EXPECT_EQ(clock.observe(make_frame_id(2, source_pts), PROGRAM_ORIGIN + FRAME_DURATION + JITTER),
              media::source_clock_observation_e::updated);
    EXPECT_EQ(require_value(clock.map(source_pts)), PROGRAM_ORIGIN + FRAME_DURATION + ADJUSTMENT);
}

TEST(SourceClock, ReanchorsAfterEpochAndSequenceDiscontinuities)
{
    constexpr auto SOURCE_ORIGIN  = utils::to_flicks(11.0);
    constexpr auto PROGRAM_ORIGIN = utils::to_flicks(3.0);

    media::source_clock_estimator_s clock;
    EXPECT_EQ(clock.observe(make_frame_id(10, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::source_clock_observation_e::initialized);

    const auto new_source_pts  = utils::to_flicks(50.0);
    const auto new_program_pts = utils::to_flicks(8.0);
    EXPECT_EQ(clock.observe(make_frame_id(1, new_source_pts, 2), new_program_pts),
              media::source_clock_observation_e::discontinuity);
    EXPECT_EQ(require_value(clock.map(new_source_pts)), new_program_pts);

    const auto restarted_source_pts  = utils::to_flicks(1.0);
    const auto restarted_program_pts = utils::to_flicks(9.0);
    EXPECT_EQ(clock.observe(make_frame_id(1, restarted_source_pts, 2), restarted_program_pts),
              media::source_clock_observation_e::discontinuity);
    EXPECT_EQ(require_value(clock.map(restarted_source_pts)), restarted_program_pts);
}

TEST(SourceClock, TracksSustainedSourceDriftWithoutFollowingIndividualFrames)
{
    constexpr auto SOURCE_ORIGIN   = utils::to_flicks(20.0);
    constexpr auto PROGRAM_ORIGIN  = utils::to_flicks(4.0);
    constexpr auto DRIFT_PER_FRAME = utils::to_flicks(0.00001);

    media::source_clock_estimator_s clock;
    ASSERT_EQ(clock.observe(make_frame_id(1, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::source_clock_observation_e::initialized);

    utils::flicks final_source_pts;
    utils::flicks final_observation;
    for (uint64_t frame = 1; frame <= 1'000; ++frame) {
        final_source_pts  = SOURCE_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        final_observation = PROGRAM_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(frame) +
                            DRIFT_PER_FRAME * static_cast<utils::flicks::rep>(frame);
        ASSERT_EQ(clock.observe(make_frame_id(frame + 1, final_source_pts), final_observation),
                  media::source_clock_observation_e::updated);
    }

    EXPECT_LT(std::chrono::abs(require_value(clock.map(final_source_pts)) - final_observation),
              utils::to_flicks(0.001));
    const auto expected_rate =
        1.0 + (static_cast<double>(DRIFT_PER_FRAME.count()) / static_cast<double>(FRAME_DURATION.count()));
    EXPECT_NEAR(require_value(clock.recovered_rate()), expected_rate, 0.0001);
}

TEST(SourceClock, ChangesRecoveredRateWithoutMovingTheCurrentPhase)
{
    constexpr auto DRIFT_PER_FRAME = utils::to_flicks(0.00001);

    media::source_clock_estimator_s clock({
        .phase_filter_divisor       = 1,
        .rate_filter_divisor        = 1,
        .rate_observation_frames    = 2,
        .maximum_rate_deviation_ppm = 5'000.0,
        .maximum_phase_adjustment   = {},
        .discontinuity_threshold    = utils::to_flicks(0.5),
    });

    ASSERT_EQ(clock.observe(make_frame_id(1, {}), {}), media::source_clock_observation_e::initialized);
    ASSERT_EQ(clock.observe(make_frame_id(2, FRAME_DURATION), FRAME_DURATION + DRIFT_PER_FRAME),
              media::source_clock_observation_e::updated);

    const auto current_source_pts = FRAME_DURATION * 2;
    ASSERT_EQ(clock.observe(make_frame_id(3, current_source_pts), current_source_pts + (DRIFT_PER_FRAME * 2)),
              media::source_clock_observation_e::updated);
    EXPECT_EQ(require_value(clock.map(current_source_pts)), current_source_pts);
    EXPECT_EQ(require_value(clock.map(current_source_pts + FRAME_DURATION)),
              current_source_pts + FRAME_DURATION + DRIFT_PER_FRAME);
}

TEST(TimedSourceQueue, SelectsNewestEligibleFrameAndThenRepeatsIt)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto first = queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(first);
    queue.advance({}, TARGET_TIME);

    const auto first_ticket = queue.select({});
    ASSERT_EQ(first_ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(first_ticket.frame(), first);
    EXPECT_TRUE(first->mark_submitted());
    EXPECT_TRUE(first->mark_ready());
    EXPECT_TRUE(first_ticket.await());
    EXPECT_TRUE(queue.commit(first_ticket));

    const auto second =
        queue.create_frame(make_frame_id(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20);
    const auto third =
        queue.create_frame(make_frame_id(3, SOURCE_ORIGIN + FRAME_DURATION * 2), TARGET_TIME + FRAME_DURATION * 2, 30);
    queue.push(second);
    queue.push(third);
    queue.advance(FRAME_DURATION * 2, TARGET_TIME + FRAME_DURATION * 2);

    const auto third_ticket = queue.select(FRAME_DURATION * 2);
    ASSERT_EQ(third_ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(third_ticket.frame(), third);
    EXPECT_TRUE(third->mark_submitted());
    EXPECT_TRUE(third->mark_ready());
    EXPECT_TRUE(third_ticket.await());
    EXPECT_TRUE(queue.commit(third_ticket));

    const auto repeat = queue.select(FRAME_DURATION * 3);
    EXPECT_EQ(repeat.selection(), media::prepared_frame_selection_e::repeat);
    EXPECT_EQ(repeat.frame(), third);
    EXPECT_TRUE(repeat.await());
    EXPECT_TRUE(queue.commit(repeat));

    const auto metrics = queue.metrics();
    EXPECT_EQ(metrics.selection_drops, 1);
    EXPECT_EQ(metrics.repeated, 1);
}

TEST(TimedSourceQueue, AwaitUsesTheExactSelectedFrameInsteadOfThePreviousFrame)
{
    using namespace std::chrono_literals;

    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto previous = queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(previous);
    queue.advance({}, TARGET_TIME);
    const auto previous_ticket = queue.select({});
    ASSERT_TRUE(previous->mark_submitted());
    ASSERT_TRUE(previous->mark_ready());
    ASSERT_TRUE(previous_ticket.await());
    ASSERT_TRUE(queue.commit(previous_ticket));

    const auto selected =
        queue.create_frame(make_frame_id(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20);
    queue.push(selected);
    queue.advance(FRAME_DURATION, TARGET_TIME + FRAME_DURATION);
    const auto selected_ticket = queue.select(FRAME_DURATION);
    ASSERT_EQ(selected_ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(selected_ticket.frame(), selected);
    ASSERT_TRUE(selected->mark_submitted());

    std::promise<void> entered_wait;
    auto               waiting = std::async(std::launch::async, [&selected_ticket, &entered_wait] {
        entered_wait.set_value();
        return selected_ticket.await();
    });
    entered_wait.get_future().wait();
    EXPECT_EQ(waiting.wait_for(10ms), std::future_status::timeout);

    ASSERT_TRUE(selected->mark_ready());
    EXPECT_TRUE(waiting.get());
    EXPECT_TRUE(queue.commit(selected_ticket));
    EXPECT_EQ(selected_ticket.frame()->value(), 20);
}

TEST(TimedSourceQueue, SubmittedFrameCanBeResolvedAfterAnUnexecutedTraversal)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto frame = queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(frame);
    queue.advance({}, TARGET_TIME);

    {
        const auto unexecuted_ticket = queue.select({});
        ASSERT_EQ(unexecuted_ticket.selection(), media::prepared_frame_selection_e::new_frame);
        ASSERT_TRUE(frame->mark_submitted());
    }

    const auto later_ticket = queue.select({});
    EXPECT_EQ(later_ticket.frame(), frame);
    EXPECT_EQ(frame->readiness(), media::source_frame_readiness_e::submitted);
    ASSERT_TRUE(frame->mark_ready());
    EXPECT_TRUE(later_ticket.await());
    EXPECT_TRUE(queue.commit(later_ticket));
}

TEST(TimedSourceQueue, ReportsFailureForTheExactSelectedFrame)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto selected = queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 20);
    queue.push(selected);
    queue.advance({}, TARGET_TIME);
    const auto ticket = queue.select({});
    ASSERT_TRUE(selected->mark_submitted());

    queue.fail(ticket);
    EXPECT_FALSE(ticket.await());
    EXPECT_FALSE(queue.commit(ticket));
    EXPECT_EQ(queue.metrics().transfer_failures, 1);

    const auto missing = queue.select(FRAME_DURATION);
    EXPECT_EQ(missing.selection(), media::prepared_frame_selection_e::missing);
    EXPECT_EQ(missing.frame(), nullptr);
}

TEST(TimedSourceQueue, ResetCancelsAnExactOutstandingTicket)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto selected = queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 20);
    queue.push(selected);
    queue.advance({}, TARGET_TIME);
    const auto ticket = queue.select({});
    ASSERT_TRUE(selected->mark_submitted());

    queue.reset();
    EXPECT_FALSE(ticket.await());
    EXPECT_EQ(selected->readiness(), media::source_frame_readiness_e::failed);
}

TEST(TimedSourceQueue, StaysBoundedAndInvalidatesRepeatsAtDiscontinuities)
{
    media::timed_source_queue_s<int> queue({.capacity = 2});
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    queue.push(queue.create_frame(make_frame_id(1, SOURCE_ORIGIN), TARGET_TIME, 10));
    queue.push(queue.create_frame(make_frame_id(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20));
    queue.push(
        queue.create_frame(make_frame_id(3, SOURCE_ORIGIN + FRAME_DURATION * 2), TARGET_TIME + FRAME_DURATION * 2, 30));
    EXPECT_EQ(queue.metrics().overflow_drops, 1);

    queue.advance(FRAME_DURATION * 2, TARGET_TIME + FRAME_DURATION * 2);
    const auto current = queue.select(FRAME_DURATION * 2);
    ASSERT_EQ(current.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_TRUE(current.frame()->mark_submitted());
    ASSERT_TRUE(current.frame()->mark_ready());
    ASSERT_TRUE(current.await());
    ASSERT_TRUE(queue.commit(current));

    const auto new_epoch =
        queue.create_frame(make_frame_id(1, utils::to_flicks(5.0), 2), TARGET_TIME + FRAME_DURATION * 3, 40);
    queue.push(new_epoch);
    queue.advance(FRAME_DURATION * 3, TARGET_TIME + FRAME_DURATION * 3);
    const auto after_discontinuity = queue.select(FRAME_DURATION * 3);
    EXPECT_TRUE(after_discontinuity.discontinuity());
    EXPECT_EQ(after_discontinuity.selection(), media::prepared_frame_selection_e::new_frame);
    EXPECT_EQ(after_discontinuity.frame(), new_epoch);
    EXPECT_EQ(queue.metrics().discontinuities, 1);
}

TEST(TimedSourceQueue, RepeatsDeterministicallyWhenTheSourceRateIsLower)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    for (uint64_t program_frame = 0; program_frame < 8; ++program_frame) {
        const auto program_pts = FRAME_DURATION * static_cast<utils::flicks::rep>(program_frame);
        const auto target_time = TARGET_TIME + program_pts;
        if (program_frame % 2 == 0) {
            const auto source_frame = program_frame / 2;
            const auto source_pts   = SOURCE_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(program_frame);
            queue.push(queue.create_frame(
                make_frame_id(source_frame + 1, source_pts), target_time, static_cast<int>(source_frame)));
        }

        queue.advance(program_pts, target_time);
        const auto ticket = queue.select(program_pts);
        if (program_frame % 2 == 0) {
            ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame);
            ASSERT_TRUE(ticket.frame()->mark_submitted());
            ASSERT_TRUE(ticket.frame()->mark_ready());
        } else {
            ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::repeat);
        }
        ASSERT_TRUE(ticket.await());
        ASSERT_TRUE(queue.commit(ticket));
        EXPECT_EQ(ticket.frame()->value(), static_cast<int>(program_frame / 2));
    }

    EXPECT_EQ(queue.metrics().repeated, 4);
}
} // namespace
