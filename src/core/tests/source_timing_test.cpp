#include "media/media_clock.hpp"
#include "media/timed_source_queue.hpp"
#include "utils/flicks.hpp"

#include <chrono>
#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
using namespace miximus;

constexpr auto FRAME_DURATION = utils::flicks{11'771'760};

media::media_clock_sample_s make_media_clock_sample(uint64_t sequence, utils::flicks pts, uint64_t epoch = 1)
{
    return {
        .stream_epoch   = epoch,
        .frame_sequence = sequence,
        .media_pts      = pts,
        .frame_duration = FRAME_DURATION,
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

TEST(MediaToProgramClock, MapsArbitraryOriginsAndFiltersArrivalJitter)
{
    constexpr auto SOURCE_ORIGIN  = utils::to_flicks(37.0);
    constexpr auto PROGRAM_ORIGIN = utils::to_flicks(2.0);
    constexpr auto JITTER         = utils::to_flicks(0.004);
    constexpr auto ADJUSTMENT     = utils::to_flicks(0.001);

    media::media_to_program_clock_s clock({
        .phase_filter_divisor     = 4,
        .maximum_phase_adjustment = ADJUSTMENT,
        .discontinuity_threshold  = utils::to_flicks(0.5),
    });

    EXPECT_EQ(clock.observe(make_media_clock_sample(1, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::media_clock_observation_e::initialized);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(SOURCE_ORIGIN)), PROGRAM_ORIGIN);

    const auto source_pts = SOURCE_ORIGIN + FRAME_DURATION;
    EXPECT_EQ(clock.observe(make_media_clock_sample(2, source_pts), PROGRAM_ORIGIN + FRAME_DURATION + JITTER),
              media::media_clock_observation_e::updated);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(source_pts)),
              PROGRAM_ORIGIN + FRAME_DURATION + ADJUSTMENT);
}

TEST(MediaToProgramClock, ReanchorsAfterEpochAndSequenceDiscontinuities)
{
    constexpr auto SOURCE_ORIGIN  = utils::to_flicks(11.0);
    constexpr auto PROGRAM_ORIGIN = utils::to_flicks(3.0);

    media::media_to_program_clock_s clock;
    EXPECT_EQ(clock.observe(make_media_clock_sample(10, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::media_clock_observation_e::initialized);

    const auto new_source_pts  = utils::to_flicks(50.0);
    const auto new_program_pts = utils::to_flicks(8.0);
    EXPECT_EQ(clock.observe(make_media_clock_sample(1, new_source_pts, 2), new_program_pts),
              media::media_clock_observation_e::discontinuity);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(new_source_pts)), new_program_pts);

    const auto restarted_source_pts  = utils::to_flicks(1.0);
    const auto restarted_program_pts = utils::to_flicks(9.0);
    EXPECT_EQ(clock.observe(make_media_clock_sample(1, restarted_source_pts, 2), restarted_program_pts),
              media::media_clock_observation_e::discontinuity);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(restarted_source_pts)), restarted_program_pts);
}

TEST(MediaToProgramClock, TracksSustainedSourceDriftWithoutFollowingIndividualFrames)
{
    constexpr auto SOURCE_ORIGIN   = utils::to_flicks(20.0);
    constexpr auto PROGRAM_ORIGIN  = utils::to_flicks(4.0);
    constexpr auto DRIFT_PER_FRAME = utils::to_flicks(0.00001);

    media::media_to_program_clock_s clock;
    ASSERT_EQ(clock.observe(make_media_clock_sample(1, SOURCE_ORIGIN), PROGRAM_ORIGIN),
              media::media_clock_observation_e::initialized);

    utils::flicks final_source_pts;
    utils::flicks final_observation;
    for (uint64_t frame = 1; frame <= 1'000; ++frame) {
        final_source_pts  = SOURCE_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        final_observation = PROGRAM_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(frame) +
                            DRIFT_PER_FRAME * static_cast<utils::flicks::rep>(frame);
        ASSERT_EQ(clock.observe(make_media_clock_sample(frame + 1, final_source_pts), final_observation),
                  media::media_clock_observation_e::updated);
    }

    EXPECT_LT(
        std::chrono::abs(require_value(clock.map_media_pts_to_program_time(final_source_pts)) - final_observation),
        utils::to_flicks(0.001));
    const auto expected_rate =
        1.0 + (static_cast<double>(DRIFT_PER_FRAME.count()) / static_cast<double>(FRAME_DURATION.count()));
    EXPECT_NEAR(require_value(clock.recovered_rate()), expected_rate, 0.0001);
}

TEST(MediaToProgramClock, RateRegressionRejectsCallbackArrivalJitter)
{
    constexpr auto JITTER = utils::to_flicks(0.008);

    media::media_to_program_clock_s clock({
        .phase_filter_divisor       = 128,
        .rate_filter_divisor        = 1,
        .rate_observation_frames    = 600,
        .maximum_rate_deviation_ppm = 1'000.0,
        .maximum_phase_adjustment   = utils::to_flicks(0.00025),
        .discontinuity_threshold    = utils::to_flicks(0.5),
    });

    ASSERT_EQ(clock.observe(make_media_clock_sample(1, {}), JITTER), media::media_clock_observation_e::initialized);
    for (uint64_t frame = 1; frame <= 600; ++frame) {
        const auto pts    = FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        const auto jitter = frame % 2 == 0 ? JITTER : -JITTER;
        ASSERT_EQ(clock.observe(make_media_clock_sample(frame + 1, pts), pts + jitter),
                  media::media_clock_observation_e::updated);
    }

    EXPECT_NEAR(require_value(clock.observed_rate()), 1.0, 0.0001);
    EXPECT_NEAR(require_value(clock.recovered_rate()), 1.0, 0.0001);
}

TEST(MediaToProgramClock, ChangesRecoveredRateWithoutMovingTheCurrentPhase)
{
    constexpr auto DRIFT_PER_FRAME = utils::to_flicks(0.00001);

    media::media_to_program_clock_s clock({
        .phase_filter_divisor       = 1,
        .rate_filter_divisor        = 1,
        .rate_observation_frames    = 2,
        .maximum_rate_deviation_ppm = 5'000.0,
        .maximum_phase_adjustment   = {},
        .discontinuity_threshold    = utils::to_flicks(0.5),
    });

    ASSERT_EQ(clock.observe(make_media_clock_sample(1, {}), {}), media::media_clock_observation_e::initialized);
    ASSERT_EQ(clock.observe(make_media_clock_sample(2, FRAME_DURATION), FRAME_DURATION + DRIFT_PER_FRAME),
              media::media_clock_observation_e::updated);

    const auto current_source_pts = FRAME_DURATION * 2;
    ASSERT_EQ(clock.observe(make_media_clock_sample(3, current_source_pts), current_source_pts + (DRIFT_PER_FRAME * 2)),
              media::media_clock_observation_e::updated);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(current_source_pts)), current_source_pts);
    EXPECT_EQ(require_value(clock.map_media_pts_to_program_time(current_source_pts + FRAME_DURATION)),
              current_source_pts + FRAME_DURATION + DRIFT_PER_FRAME);
}

TEST(TimedSourceQueue, SelectsNewestEligibleFrameAndThenRepeatsIt)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto first = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(first);
    queue.advance({}, TARGET_TIME);

    const auto first_ticket = queue.select({});
    ASSERT_EQ(first_ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(first_ticket.frame(), first);
    EXPECT_TRUE(first->mark_submitted());
    EXPECT_TRUE(first->mark_ready());
    EXPECT_TRUE(first_ticket.await());
    EXPECT_TRUE(queue.commit(first_ticket));

    const auto second = queue.create_frame(
        make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20);
    const auto third = queue.create_frame(
        make_media_clock_sample(3, SOURCE_ORIGIN + FRAME_DURATION * 2), TARGET_TIME + FRAME_DURATION * 2, 30);
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

TEST(TimedSourceQueue, RequiresCapacityForTheDelayedFramesAndTheIncomingFrame)
{
    EXPECT_THROW((media::timed_source_queue_s<int>{
                     {.capacity = 3, .playout_delay_frames = 3}
    }),
                 std::invalid_argument);
    EXPECT_NO_THROW((media::timed_source_queue_s<int>{
        {.capacity = 4, .playout_delay_frames = 3}
    }));
}

TEST(TimedSourceQueue, AppliesPlayoutDelayInSourceFrameIntervals)
{
    media::timed_source_queue_s<int> queue({.playout_delay_frames = 1});
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto first = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(first);
    queue.advance({}, TARGET_TIME);
    EXPECT_EQ(queue.select({}).selection(), media::prepared_frame_selection_e::missing);

    const auto second = queue.create_frame(
        make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20);
    queue.push(second);
    queue.advance(FRAME_DURATION, TARGET_TIME + FRAME_DURATION);

    const auto ticket = queue.select(FRAME_DURATION);
    ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(ticket.frame(), first);
    ASSERT_TRUE(first->mark_submitted());
    ASSERT_TRUE(first->mark_ready());
    ASSERT_TRUE(ticket.await());
    EXPECT_TRUE(queue.commit(ticket));
}

TEST(TimedSourceQueue, HalfFrameToleranceRejectsArrivalJitterWithoutRepeatingOrDropping)
{
    media::timed_source_queue_s<int> queue({.playout_delay_frames = 1});
    constexpr auto                   TARGET_TIME    = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN  = utils::to_flicks(40.0);
    constexpr auto                   CALLBACK_PHASE = FRAME_DURATION / 2;
    constexpr auto                   JITTER         = utils::to_flicks(0.002);

    for (uint64_t frame = 0; frame < 300; ++frame) {
        const auto offset      = FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        const auto source_pts  = SOURCE_ORIGIN + offset;
        const auto program_pts = offset;
        const auto arrival     = TARGET_TIME + offset + CALLBACK_PHASE + (frame % 2 == 0 ? -JITTER : JITTER);
        queue.push(
            queue.create_frame(make_media_clock_sample(frame + 1, source_pts), arrival, static_cast<int>(frame)));
        queue.advance(program_pts, TARGET_TIME + program_pts);

        const auto ticket = queue.select(program_pts, FRAME_DURATION / 2);
        if (frame == 0) {
            EXPECT_EQ(ticket.selection(), media::prepared_frame_selection_e::missing);
            continue;
        }

        ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame);
        ASSERT_NE(ticket.frame(), nullptr);
        EXPECT_EQ(ticket.frame()->payload(), static_cast<int>(frame - 1));
        ASSERT_TRUE(ticket.frame()->mark_submitted());
        ASSERT_TRUE(ticket.frame()->mark_ready());
        ASSERT_TRUE(ticket.await());
        ASSERT_TRUE(queue.commit(ticket));
    }

    EXPECT_EQ(queue.metrics().selection_drops, 0);
    EXPECT_EQ(queue.metrics().repeated, 0);
}

TEST(TimedSourceQueue, DelayedExactRateSourceDoesNotOscillateAtTheHalfFrameBoundary)
{
    constexpr auto EXACT_FRAME_DURATION = utils::flicks{11'760'000};
    constexpr auto TARGET_TIME          = utils::to_flicks(100.0);
    constexpr auto SOURCE_ORIGIN        = utils::to_flicks(40.0);
    constexpr auto CALLBACK_PHASE       = EXACT_FRAME_DURATION / 2;
    constexpr auto JITTER               = utils::to_flicks(0.0001);

    media::timed_source_queue_s<int> queue({.capacity = 5, .playout_delay_frames = 3});
    for (uint64_t frame = 0; frame < 10'000; ++frame) {
        const auto offset             = EXACT_FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        const auto program_pts        = offset;
        const auto media_clock_sample = media::media_clock_sample_s{
            .stream_epoch   = 1,
            .frame_sequence = frame + 1,
            .media_pts      = SOURCE_ORIGIN + offset,
            .frame_duration = EXACT_FRAME_DURATION,
        };
        const auto jitter = frame % 10 == 0 ? JITTER : -JITTER;
        queue.push(queue.create_frame(
            media_clock_sample, TARGET_TIME + offset + CALLBACK_PHASE + jitter, static_cast<int>(frame)));
        queue.advance(program_pts, TARGET_TIME + program_pts);

        const auto ticket = queue.select(program_pts, EXACT_FRAME_DURATION / 2);
        if (ticket.selection() == media::prepared_frame_selection_e::missing) {
            continue;
        }
        ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame) << "at frame " << frame;
        ASSERT_NE(ticket.frame(), nullptr);
        ASSERT_TRUE(ticket.frame()->mark_submitted());
        ASSERT_TRUE(ticket.frame()->mark_ready());
        ASSERT_TRUE(ticket.await());
        ASSERT_TRUE(queue.commit(ticket));
    }

    EXPECT_EQ(queue.metrics().timing_repeats, 0);
    EXPECT_EQ(queue.metrics().overflow_drops, 0);
}

TEST(TimedSourceQueue, FasterNominalSourceDoesNotDevelopCompensatingRepeatsDuringLongRuns)
{
    constexpr auto SOURCE_FRAME_DURATION  = utils::flicks{11'760'000};
    constexpr auto PROGRAM_FRAME_DURATION = utils::flicks{11'771'760};
    constexpr auto TARGET_TIME            = utils::to_flicks(100.0);
    constexpr auto SOURCE_ORIGIN          = utils::to_flicks(40.0);
    constexpr auto CALLBACK_PHASE         = SOURCE_FRAME_DURATION / 2;
    constexpr auto CALLBACK_JITTER        = utils::to_flicks(0.002);
    constexpr auto PROGRAM_FRAME_COUNT    = 432'000ULL;
    constexpr auto SOURCE_CLOCK_SCALE     = 1.000'010L;

    media::timed_source_queue_s<int> queue({.capacity = 8, .playout_delay_frames = 3});
    uint64_t                         next_source_frame = 0;

    for (uint64_t program_frame = 0; program_frame < PROGRAM_FRAME_COUNT; ++program_frame) {
        const auto program_pts         = PROGRAM_FRAME_DURATION * static_cast<utils::flicks::rep>(program_frame);
        const auto program_target_time = TARGET_TIME + program_pts;

        while (true) {
            const auto source_offset  = SOURCE_FRAME_DURATION * static_cast<utils::flicks::rep>(next_source_frame);
            const auto arrival_offset = utils::flicks{static_cast<utils::flicks::rep>(
                std::llround(static_cast<long double>(source_offset.count()) * SOURCE_CLOCK_SCALE))};
            const auto jitter         = next_source_frame % 2 == 0 ? CALLBACK_JITTER : -CALLBACK_JITTER;
            const auto arrival_time   = TARGET_TIME + CALLBACK_PHASE + arrival_offset + jitter;
            if (arrival_time > program_target_time) {
                break;
            }

            queue.push(queue.create_frame(
                {
                    .stream_epoch   = 1,
                    .frame_sequence = next_source_frame + 1,
                    .media_pts      = SOURCE_ORIGIN + source_offset,
                    .frame_duration = SOURCE_FRAME_DURATION,
                },
                arrival_time,
                static_cast<int>(next_source_frame)));
            ++next_source_frame;
        }

        queue.advance(program_pts, program_target_time);
        const auto ticket = queue.select(program_pts, PROGRAM_FRAME_DURATION / 2);
        if (ticket.selection() == media::prepared_frame_selection_e::missing) {
            continue;
        }
        if (ticket.selection() == media::prepared_frame_selection_e::repeat) {
            ASSERT_TRUE(ticket.await());
            ASSERT_TRUE(queue.commit(ticket));
            continue;
        }
        ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame) << "at frame " << program_frame;
        ASSERT_NE(ticket.frame(), nullptr);
        ASSERT_TRUE(ticket.frame()->mark_submitted());
        ASSERT_TRUE(ticket.frame()->mark_ready());
        ASSERT_TRUE(ticket.await());
        ASSERT_TRUE(queue.commit(ticket));
    }

    const auto metrics = queue.metrics();
    EXPECT_GE(metrics.selection_drops, 400);
    EXPECT_LE(metrics.selection_drops, 450);
    EXPECT_EQ(metrics.repeated, 0);
    EXPECT_EQ(metrics.starvation_repeats, 0);
    EXPECT_EQ(metrics.timing_repeats, 0);
    EXPECT_EQ(metrics.overflow_drops, 0);
}

TEST(TimedSourceQueue, AwaitUsesTheExactSelectedFrameInsteadOfThePreviousFrame)
{
    using namespace std::chrono_literals;

    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto previous = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10);
    queue.push(previous);
    queue.advance({}, TARGET_TIME);
    const auto previous_ticket = queue.select({});
    ASSERT_TRUE(previous->mark_submitted());
    ASSERT_TRUE(previous->mark_ready());
    ASSERT_TRUE(previous_ticket.await());
    ASSERT_TRUE(queue.commit(previous_ticket));

    const auto selected = queue.create_frame(
        make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20);
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
    EXPECT_EQ(selected_ticket.frame()->payload(), 20);
}

TEST(TimedSourceQueue, SubmittedFrameCanBeResolvedAfterAnUnexecutedTraversal)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto frame = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10);
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

TEST(TimedSourceQueue, NeverSelectsAnOlderSourceFrameAfterClockCorrectionReordersMappedTimes)
{
    media::timed_source_queue_s<int> queue({
        .clock =
            {
                    .phase_filter_divisor     = 1,
                    .maximum_phase_adjustment = utils::to_flicks(1.0),
                    .discontinuity_threshold  = utils::to_flicks(1.0),
                    },
    });
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto older =
        queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME + utils::to_flicks(0.1), 10);
    const auto newer = queue.create_frame(make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME, 20);
    queue.push(older);
    queue.push(newer);
    queue.advance({}, TARGET_TIME);

    const auto selected = queue.select({});
    ASSERT_EQ(selected.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_EQ(selected.frame(), newer);
    ASSERT_TRUE(newer->mark_submitted());
    ASSERT_TRUE(newer->mark_ready());
    ASSERT_TRUE(queue.commit(selected));

    const auto later = queue.select(utils::to_flicks(0.1));
    EXPECT_EQ(later.selection(), media::prepared_frame_selection_e::repeat);
    EXPECT_EQ(later.frame(), newer);
    EXPECT_EQ(older->readiness(), media::source_frame_readiness_e::failed);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
}

TEST(TimedSourceQueue, ReportsFailureForTheExactSelectedFrame)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto selected = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 20);
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

TEST(TimedSourceQueue, ReportsCancellationSeparatelyFromTransferFailure)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto selected = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 20);
    queue.push(selected);
    queue.advance({}, TARGET_TIME);
    const auto ticket = queue.select({});
    ASSERT_TRUE(selected->mark_submitted());

    queue.cancel(ticket);
    EXPECT_FALSE(ticket.await());
    EXPECT_FALSE(queue.commit(ticket));
    EXPECT_EQ(queue.metrics().transfer_failures, 0);
    EXPECT_EQ(queue.metrics().transfer_cancellations, 1);
}

TEST(TimedSourceQueue, ResetCancelsAnExactOutstandingTicket)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    const auto selected = queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 20);
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

    queue.push(queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10));
    queue.push(queue.create_frame(
        make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20));
    queue.push(queue.create_frame(
        make_media_clock_sample(3, SOURCE_ORIGIN + FRAME_DURATION * 2), TARGET_TIME + FRAME_DURATION * 2, 30));
    EXPECT_EQ(queue.metrics().overflow_drops, 1);

    queue.advance(FRAME_DURATION * 2, TARGET_TIME + FRAME_DURATION * 2);
    const auto current = queue.select(FRAME_DURATION * 2);
    ASSERT_EQ(current.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_TRUE(current.frame()->mark_submitted());
    ASSERT_TRUE(current.frame()->mark_ready());
    ASSERT_TRUE(current.await());
    ASSERT_TRUE(queue.commit(current));

    const auto new_epoch =
        queue.create_frame(make_media_clock_sample(1, utils::to_flicks(5.0), 2), TARGET_TIME + FRAME_DURATION * 3, 40);
    queue.push(new_epoch);
    queue.advance(FRAME_DURATION * 3, TARGET_TIME + FRAME_DURATION * 3);
    const auto after_discontinuity = queue.select(FRAME_DURATION * 3);
    EXPECT_TRUE(after_discontinuity.discontinuity());
    EXPECT_EQ(after_discontinuity.selection(), media::prepared_frame_selection_e::new_frame);
    EXPECT_EQ(after_discontinuity.frame(), new_epoch);
    EXPECT_EQ(queue.metrics().discontinuities, 1);
}

TEST(TimedSourceQueue, CapacityAppliesAcrossPendingAndAlignedFrames)
{
    media::timed_source_queue_s<int> queue({.capacity = 2});
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    queue.push(queue.create_frame(make_media_clock_sample(1, SOURCE_ORIGIN), TARGET_TIME, 10));
    queue.push(queue.create_frame(
        make_media_clock_sample(2, SOURCE_ORIGIN + FRAME_DURATION), TARGET_TIME + FRAME_DURATION, 20));
    queue.advance(FRAME_DURATION, TARGET_TIME + FRAME_DURATION);

    queue.push(queue.create_frame(
        make_media_clock_sample(3, SOURCE_ORIGIN + FRAME_DURATION * 2), TARGET_TIME + FRAME_DURATION * 2, 30));
    EXPECT_EQ(queue.metrics().queued, 2);
    EXPECT_EQ(queue.metrics().overflow_drops, 1);

    queue.advance(FRAME_DURATION * 2, TARGET_TIME + FRAME_DURATION * 2);
    const auto ticket = queue.select(FRAME_DURATION * 2);
    ASSERT_EQ(ticket.selection(), media::prepared_frame_selection_e::new_frame);
    ASSERT_TRUE(ticket.frame()->mark_submitted());
    ASSERT_TRUE(ticket.frame()->mark_ready());
    ASSERT_TRUE(ticket.await());
    ASSERT_TRUE(queue.commit(ticket));
    EXPECT_EQ(ticket.frame()->payload(), 20);
    EXPECT_EQ(queue.metrics().queued, 0);
}

TEST(TimedSourceQueue, RepeatsDeterministicallyWhenTheSourceRateIsLower)
{
    media::timed_source_queue_s<int> queue;
    constexpr auto                   TARGET_TIME   = utils::to_flicks(100.0);
    constexpr auto                   SOURCE_ORIGIN = utils::to_flicks(40.0);

    for (uint64_t program_frame = 0; program_frame < 8; ++program_frame) {
        const auto program_pts         = FRAME_DURATION * static_cast<utils::flicks::rep>(program_frame);
        const auto program_target_time = TARGET_TIME + program_pts;
        if (program_frame % 2 == 0) {
            const auto source_frame = program_frame / 2;
            const auto source_pts   = SOURCE_ORIGIN + FRAME_DURATION * static_cast<utils::flicks::rep>(program_frame);
            queue.push(queue.create_frame(make_media_clock_sample(source_frame + 1, source_pts),
                                          program_target_time,
                                          static_cast<int>(source_frame)));
        }

        queue.advance(program_pts, program_target_time);
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
        EXPECT_EQ(ticket.frame()->payload(), static_cast<int>(program_frame / 2));
    }

    EXPECT_EQ(queue.metrics().repeated, 4);
}
} // namespace
