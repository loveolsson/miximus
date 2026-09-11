#include "media/media_clock.hpp"
#include "media/output_runtime_metrics.hpp"
#include "media/playout_timeline.hpp"
#include "media/presentation_clock.hpp"
#include "media/presentation_timeline.hpp"
#include "media/timed_output_queue.hpp"
#include "utils/flicks.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {
using namespace miximus;

media::output_frame_s<int> make_frame(utils::flicks program_target_time, int value)
{
    return {
        .program_target_time = program_target_time,
        .payload             = value,
    };
}

TEST(TimedOutputQueue, SelectsNewestEligibleFrameAndRepeatsIt)
{
    media::timed_output_queue_s<int> queue;
    queue.push(make_frame(utils::to_flicks(0.00), 10));
    queue.push(make_frame(utils::to_flicks(0.01), 20));
    queue.push(make_frame(utils::to_flicks(0.02), 30));

    const auto selected = queue.select(utils::to_flicks(0.015));
    ASSERT_EQ(selected.selection, media::output_frame_selection_e::new_frame);
    ASSERT_NE(selected.frame, nullptr);
    EXPECT_EQ(selected.frame->payload, 20);

    const auto repeat = queue.select(utils::to_flicks(0.016));
    ASSERT_EQ(repeat.selection, media::output_frame_selection_e::repeat);
    ASSERT_NE(repeat.frame, nullptr);
    EXPECT_EQ(repeat.frame->payload, 20);

    EXPECT_EQ(queue.metrics().selection_drops, 1);
    EXPECT_EQ(queue.metrics().repeated, 1);
}

TEST(TimedOutputQueue, SelectsByAbsoluteProgramTargetTime)
{
    media::timed_output_queue_s<int> queue;
    queue.push({
        .program_target_time = utils::to_flicks(100.0),
        .payload             = 10,
    });
    queue.push({
        .program_target_time = utils::to_flicks(101.0),
        .payload             = 20,
    });

    const auto selected = queue.select(utils::to_flicks(100.5));
    ASSERT_NE(selected.frame, nullptr);
    EXPECT_EQ(selected.frame->payload, 10);
}

TEST(TimedOutputQueue, ConvertsSlowerProgramCadenceWithExplicitRepeats)
{
    constexpr auto SOURCE_DURATION = utils::to_flicks(1.0 / 50.0);
    constexpr auto OUTPUT_DURATION = utils::to_flicks(1.0 / 60.0);

    media::timed_output_queue_s<int> queue({.capacity = 64, .early_tolerance = SOURCE_DURATION / 2});
    for (uint64_t frame = 0; frame < 50; ++frame) {
        queue.push(make_frame(SOURCE_DURATION * static_cast<utils::flicks::rep>(frame), static_cast<int>(frame)));
    }

    uint64_t new_frames{};
    uint64_t repeats{};
    for (uint64_t slot = 0; slot < 60; ++slot) {
        const auto selection = queue.select(OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot));
        new_frames += selection.selection == media::output_frame_selection_e::new_frame ? 1 : 0;
        repeats += selection.selection == media::output_frame_selection_e::repeat ? 1 : 0;
    }

    EXPECT_EQ(new_frames, 50);
    EXPECT_EQ(repeats, 10);
}

TEST(TimedOutputQueue, ConvertsFasterProgramCadenceWithExplicitDrops)
{
    constexpr auto SOURCE_DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto OUTPUT_DURATION = utils::to_flicks(1.0 / 50.0);

    media::timed_output_queue_s<int> queue({.capacity = 64, .early_tolerance = SOURCE_DURATION / 2});
    for (uint64_t frame = 0; frame < 60; ++frame) {
        queue.push(make_frame(SOURCE_DURATION * static_cast<utils::flicks::rep>(frame), static_cast<int>(frame)));
    }

    for (uint64_t slot = 0; slot < 50; ++slot) {
        EXPECT_EQ(queue.select(OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot)).selection,
                  media::output_frame_selection_e::new_frame);
    }
    EXPECT_EQ(queue.metrics().selection_drops, 10);
}

TEST(TimedOutputQueue, DiscardsLateCompletionWithoutRegressingTheOutput)
{
    media::timed_output_queue_s<int> queue;
    queue.push(make_frame(utils::to_flicks(0.02), 20));
    ASSERT_EQ(queue.select(utils::to_flicks(0.02)).selection, media::output_frame_selection_e::new_frame);

    queue.push(make_frame(utils::to_flicks(0.01), 10));
    const auto selection = queue.select(utils::to_flicks(0.03));
    ASSERT_EQ(selection.selection, media::output_frame_selection_e::repeat);
    ASSERT_NE(selection.frame, nullptr);
    EXPECT_EQ(selection.frame->payload, 20);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
}

TEST(TimedOutputQueue, DropsOldestQueuedFramesAtCapacity)
{
    media::timed_output_queue_s<int> queue({.capacity = 2});
    queue.push(make_frame(utils::to_flicks(0.01), 10));
    queue.push(make_frame(utils::to_flicks(0.02), 20));
    queue.push(make_frame(utils::to_flicks(0.03), 30));

    const auto selection = queue.select(utils::to_flicks(0.03));
    ASSERT_NE(selection.frame, nullptr);
    EXPECT_EQ(selection.frame->payload, 30);
    EXPECT_EQ(queue.metrics().overflow_drops, 1);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
}

TEST(TimedOutputQueue, ReportsOldestRetainedProgramTargetTimeAfterOverflow)
{
    media::timed_output_queue_s<int> queue({.capacity = 2});
    EXPECT_EQ(queue.capacity(), 2);
    EXPECT_FALSE(queue.oldest_program_target_time().has_value());

    queue.push(make_frame(utils::to_flicks(0.01), 10));
    queue.push(make_frame(utils::to_flicks(0.02), 20));
    queue.push(make_frame(utils::to_flicks(0.03), 30));

    const auto oldest_program_target_time = queue.oldest_program_target_time();
    const auto oldest_target              = oldest_program_target_time.value_or(utils::flicks{});
    ASSERT_TRUE(oldest_program_target_time.has_value());
    EXPECT_EQ(oldest_target, utils::to_flicks(0.02));
}

TEST(PresentationTimeline, PreservesBufferedLatencyAcrossPresentationClockProgress)
{
    media::presentation_timeline_s timeline;
    const auto                     presentation = utils::to_flicks(100.0);
    const auto                     program      = utils::to_flicks(99.9);

    timeline.observe_latency(presentation, program);

    EXPECT_EQ(timeline.latency(), utils::to_flicks(0.1));
    EXPECT_EQ(timeline.map_presentation_to_program_target(presentation + utils::to_flicks(10.0)),
              program + utils::to_flicks(10.0));
}

TEST(PresentationTimeline, StartupObservationRollsOutOfTheLatencyAverage)
{
    media::presentation_timeline_s timeline;
    const auto                     presentation = utils::to_flicks(100.0);

    timeline.observe_latency(presentation, presentation - utils::to_flicks(0.4));
    for (size_t observation = 0; observation < 16; ++observation) {
        timeline.observe_latency(presentation, presentation - utils::to_flicks(0.1));
    }

    EXPECT_EQ(timeline.latency(), utils::to_flicks(0.1));
}

TEST(PresentationTimeline, RetainedFramesSmoothlyCorrectADelayedStartupObservation)
{
    constexpr auto FRAME_DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto RETAINED_DELAY = utils::to_flicks(0.1);
    const auto     origin         = utils::to_flicks(100.0);

    media::presentation_timeline_s timeline;
    timeline.observe_latency(origin, origin - utils::to_flicks(0.4));

    for (size_t frame = 1; frame <= 64; ++frame) {
        const auto presentation = origin + FRAME_DURATION * static_cast<utils::flicks::rep>(frame);
        const auto latency      = timeline.latency();
        if (!latency.has_value()) {
            ADD_FAILURE() << "Expected a retained latency observation";
            return;
        }
        timeline.observe_latency(presentation, presentation - *latency);

        const auto oldest_retained = presentation - RETAINED_DELAY;
        const auto requested       = timeline.map_presentation_to_program_target(presentation);
        if (!requested.has_value()) {
            ADD_FAILURE() << "Expected a mapped presentation target";
            return;
        }
        if (*requested + FRAME_DURATION / 2 < oldest_retained) {
            timeline.observe_latency(presentation, oldest_retained);
        }
    }

    const auto latency = timeline.latency();
    if (!latency.has_value()) {
        ADD_FAILURE() << "Expected a retained latency observation";
        return;
    }
    EXPECT_LT(*latency, RETAINED_DELAY + FRAME_DURATION);
    EXPECT_GT(*latency, RETAINED_DELAY - FRAME_DURATION);
}

TEST(PresentationTimeline, ConvertsSixtyFpsProgramToNtscOutputWithoutTimelineDrift)
{
    constexpr auto PROGRAM_DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto OUTPUT_DURATION  = utils::flicks{11'771'760};
    constexpr auto ORIGIN           = utils::to_flicks(1'000.0);

    media::timed_output_queue_s<int> queue({.capacity = 1'100, .early_tolerance = PROGRAM_DURATION / 2});
    for (uint64_t frame = 0; frame < 1'100; ++frame) {
        const auto target = ORIGIN + PROGRAM_DURATION * static_cast<utils::flicks::rep>(frame);
        queue.push({
            .program_target_time = target,
            .payload             = static_cast<int>(frame),
        });
    }

    media::presentation_timeline_s timeline;
    timeline.observe_latency(ORIGIN + utils::to_flicks(0.1), ORIGIN);
    for (uint64_t slot = 0; slot < 1'001; ++slot) {
        const auto presentation =
            ORIGIN + utils::to_flicks(0.1) + OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot);
        const auto target              = timeline.map_presentation_to_program_target(presentation);
        const auto program_target_time = target.value_or(utils::flicks{});
        ASSERT_TRUE(target.has_value());
        ASSERT_NE(queue.select(program_target_time).frame, nullptr);
    }

    EXPECT_EQ(queue.metrics().selection_drops, 1);
    EXPECT_EQ(timeline.latency(), utils::to_flicks(0.1));
}

TEST(PresentationTimeline, FiltersCallbackJitterBeforeConvertingNtscProgramToSixtyFpsOutput)
{
    constexpr auto PROGRAM_DURATION = utils::flicks{11'771'760};
    constexpr auto OUTPUT_DURATION  = utils::to_flicks(1.0 / 60.0);
    constexpr auto PROGRAM_ORIGIN   = utils::to_flicks(1'000.0);
    constexpr auto STEADY_ORIGIN    = utils::to_flicks(2'000.0);
    constexpr auto JITTER           = utils::to_flicks(0.0002);
    constexpr auto FRAME_COUNT      = uint64_t{6'000};

    media::timed_output_queue_s<int> queue({.capacity = FRAME_COUNT, .early_tolerance = PROGRAM_DURATION / 2});
    for (uint64_t frame = 0; frame < FRAME_COUNT; ++frame) {
        queue.push(make_frame(PROGRAM_ORIGIN + PROGRAM_DURATION * static_cast<utils::flicks::rep>(frame),
                              static_cast<int>(frame)));
    }

    media::media_to_program_clock_s output_clock;
    media::presentation_timeline_s  timeline;
    for (uint64_t slot = 0; slot < FRAME_COUNT; ++slot) {
        const auto output_time = OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot);
        const auto jitter      = slot % 2 == 0 ? JITTER : -JITTER;
        output_clock.observe(
            {
                .stream_epoch   = 1,
                .frame_sequence = slot,
                .media_pts      = output_time,
                .frame_duration = OUTPUT_DURATION,
            },
            STEADY_ORIGIN + output_time + jitter);

        const auto presentation = output_clock.map_media_pts_to_program_time(output_time).value_or(utils::flicks{});
        if (slot == 0) {
            timeline.observe_latency(presentation, PROGRAM_ORIGIN);
        }
        const auto program_target = timeline.map_presentation_to_program_target(presentation).value_or(utils::flicks{});
        ASSERT_NE(queue.select(program_target).frame, nullptr);
    }

    EXPECT_EQ(queue.metrics().repeated, 6);
    EXPECT_EQ(queue.metrics().selection_drops, 0);
}

TEST(OutputRuntimeMetrics, SeparatesCadenceRepeatsFromStarvation)
{
    media::output_runtime_metrics_s metrics;

    metrics.observe_selection(media::output_frame_selection_e::repeat, true);
    metrics.observe_selection(media::output_frame_selection_e::repeat, false);
    metrics.observe_selection(media::output_frame_selection_e::repeat, false);
    metrics.observe_selection(media::output_frame_selection_e::new_frame, false);

    const auto snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.cadence_repeats, 1);
    EXPECT_EQ(snapshot.starvation_repeats, 2);
    EXPECT_EQ(snapshot.starvation_repeat_streak, 0);
    EXPECT_EQ(snapshot.starvation_repeat_streak_max, 2);
}

TEST(OutputRuntimeMetrics, RetainsCompletionBufferAndQueueExtrema)
{
    media::output_runtime_metrics_s metrics;
    const auto                      start = utils::flicks{};

    metrics.observe_completion(start);
    metrics.observe_completion(start + std::chrono::milliseconds(20));
    metrics.observe_completion(start + std::chrono::milliseconds(35));
    metrics.observe_output_queue_depth(2);
    metrics.observe_output_queue_depth(5);
    metrics.observe_output_queue_depth(1);
    metrics.observe_buffered_frames(4, 4);
    metrics.observe_buffered_frames(2, 4);
    metrics.observe_buffered_frames(0, 4);
    metrics.observe_refill(2, 1);

    const auto snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.completion_intervals, 2);
    EXPECT_EQ(snapshot.completion_interval_max, std::chrono::milliseconds(20));
    EXPECT_EQ(snapshot.output_queue_depth, 1);
    EXPECT_EQ(snapshot.output_queue_depth_max, 5);
    EXPECT_EQ(snapshot.buffered_frames_min, 0);
    EXPECT_EQ(snapshot.buffered_frames_max, 4);
    EXPECT_EQ(snapshot.buffered_below_target_samples, 2);
    EXPECT_EQ(snapshot.buffered_zero_samples, 1);
    EXPECT_EQ(snapshot.refill_shortfalls, 1);
}

TEST(PresentationClock, NominalCadenceDoesNotFollowCompletionJitter)
{
    constexpr auto              DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto              ORIGIN   = utils::to_flicks(100.0);
    media::presentation_clock_s clock(media::presentation_clock_mode_e::nominal, DURATION, ORIGIN);

    for (int frame = 1; frame <= 1'000; ++frame) {
        const auto expected = ORIGIN + DURATION * frame;
        EXPECT_EQ(clock.predicted_completion(), expected);
        const auto delay  = utils::to_flicks(frame % 2 == 0 ? 0.0001 : 0.002);
        const auto result = clock.observe_completion(expected + delay);
        EXPECT_EQ(result.interval_count, 1);
        EXPECT_DOUBLE_EQ(result.refresh_hz, 60.0);
    }
}

TEST(PresentationClock, MissedNominalIntervalsAdvanceWithoutCatchUpBursts)
{
    constexpr auto              DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto              ORIGIN   = utils::to_flicks(100.0);
    media::presentation_clock_s clock(media::presentation_clock_mode_e::nominal, DURATION, ORIGIN);

    const auto result = clock.observe_completion(ORIGIN + DURATION * 4 + utils::to_flicks(0.0002));
    EXPECT_EQ(result.interval_count, 4);
    EXPECT_EQ(clock.predicted_completion(), ORIGIN + DURATION * 5);
    EXPECT_EQ(clock.observe_completion(ORIGIN + DURATION * 5).interval_count, 1);
    EXPECT_EQ(clock.predicted_completion(), ORIGIN + DURATION * 6);
}

TEST(PresentationClock, MissedObservedIntervalsDoNotSlowTheRecoveredRefreshRate)
{
    constexpr auto              DURATION = utils::to_flicks(1.0 / 60.0);
    constexpr auto              ORIGIN   = utils::to_flicks(100.0);
    media::presentation_clock_s clock(media::presentation_clock_mode_e::observed, DURATION, ORIGIN);

    for (int frame = 3; frame <= 3'000; frame += 3) {
        const auto result = clock.observe_completion(ORIGIN + DURATION * frame);
        EXPECT_EQ(result.interval_count, 3);
        EXPECT_NEAR(result.refresh_hz, 60.0, 0.001);
        EXPECT_EQ(clock.predicted_completion(), ORIGIN + DURATION * (frame + 1));
    }
}

TEST(PresentationClock, NominalCadencePreservesNtscRepeatsDespiteCompletionJitter)
{
    constexpr auto                   PROGRAM_DURATION = utils::flicks{11'771'760};
    constexpr auto                   OUTPUT_DURATION  = utils::to_flicks(1.0 / 60.0);
    constexpr auto                   ORIGIN           = utils::to_flicks(100.0);
    constexpr auto                   LATENCY          = OUTPUT_DURATION * 3;
    media::timed_output_queue_s<int> queue({.capacity = 6'000, .early_tolerance = PROGRAM_DURATION / 2});
    for (int frame = 0; frame < 6'000; ++frame) {
        queue.push(make_frame(ORIGIN + PROGRAM_DURATION * frame, frame));
    }

    ASSERT_NE(queue.select(ORIGIN).frame, nullptr);
    media::presentation_clock_s    clock(media::presentation_clock_mode_e::nominal, OUTPUT_DURATION, ORIGIN + LATENCY);
    media::presentation_timeline_s timeline;
    timeline.observe_latency(ORIGIN + LATENCY, ORIGIN);
    for (int frame = 1; frame < 6'000; ++frame) {
        const auto presentation = clock.predicted_completion();
        const auto target       = timeline.map_presentation_to_program_target(presentation);
        ASSERT_TRUE(target);
        ASSERT_NE(queue.select(target.value_or(utils::flicks{})).frame, nullptr);
        clock.observe_completion(presentation + utils::to_flicks(frame % 2 == 0 ? 0.0001 : 0.002));
    }

    EXPECT_EQ(queue.metrics().repeated, 6);
    EXPECT_EQ(queue.metrics().selection_drops, 0);
    EXPECT_EQ(timeline.latency(), LATENCY);
}

TEST(TimedOutputQueue, NearestSelectionComparesRetainedAndFutureFrames)
{
    constexpr auto                   DURATION = utils::flicks{23'520'000}; // 30 fps.
    media::timed_output_queue_s<int> queue({.capacity = 5});
    queue.push(make_frame(utils::flicks{}, 0));
    queue.push(make_frame(DURATION, 1));
    queue.push(make_frame(DURATION * 2, 2));

    ASSERT_EQ(queue.select_nearest(utils::flicks{}).frame->payload, 0);
    EXPECT_EQ(queue.select_nearest(DURATION / 2).frame->payload, 0);
    EXPECT_EQ(queue.select_nearest(utils::to_flicks(0.020)).frame->payload, 1);
    EXPECT_EQ(queue.queued(), 1);
    EXPECT_EQ(queue.select_nearest(DURATION).selection, media::output_frame_selection_e::repeat);
    EXPECT_EQ(queue.select_nearest(DURATION * 2).frame->payload, 2);
    EXPECT_EQ(queue.metrics().selection_drops, 0);
}

TEST(TimedOutputQueue, NearestSelectionReleasesObsoleteLeasesAndKeepsFutureFrames)
{
    media::timed_output_queue_s<std::shared_ptr<int>> queue({.capacity = 5});
    auto                                              obsolete = std::make_shared<int>(0);
    auto                                              selected = std::make_shared<int>(1);
    auto                                              future   = std::make_shared<int>(2);
    queue.push({.program_target_time = utils::to_flicks(0.00), .payload = obsolete});
    queue.push({.program_target_time = utils::to_flicks(0.01), .payload = selected});
    queue.push({.program_target_time = utils::to_flicks(0.02), .payload = future});

    ASSERT_NE(queue.select_nearest(utils::to_flicks(0.012)).frame, nullptr);
    EXPECT_EQ(obsolete.use_count(), 1);
    EXPECT_EQ(selected.use_count(), 2);
    EXPECT_EQ(future.use_count(), 2);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
    EXPECT_EQ(queue.queued(), 1);
}

class ScreenCadence : public testing::TestWithParam<std::pair<utils::flicks, utils::flicks>>
{
};

void verify_screen_cadence(utils::flicks program_duration,
                           utils::flicks display_duration,
                           utils::flicks notification_jitter)
{
    constexpr auto                   ORIGIN      = utils::to_flicks(100.0);
    constexpr auto                   RENDER_TIME = utils::to_flicks(0.001);
    constexpr auto                   FRAME_COUNT = 6'000;
    media::timed_output_queue_s<int> queue({.capacity = 5});
    media::playout_timeline_s        timeline;

    // Match the app: two program frames of preroll, then one display interval
    // before the first image appears. Subsequent frames arrive continuously.
    queue.push(make_frame(ORIGIN, 0));
    queue.push(make_frame(ORIGIN + program_duration, 1));
    const auto initial_presentation = ORIGIN + program_duration + RENDER_TIME + display_duration;
    timeline.initialize(initial_presentation, queue.select_nearest(ORIGIN).frame->program_target_time);
    media::presentation_clock_s clock(
        media::presentation_clock_mode_e::observed, utils::flicks{11'760'000}, initial_presentation);
    int  next_program_frame    = 2;
    int  previous_frame        = 0;
    int  previous_advance      = 1;
    auto previous_presentation = initial_presentation;

    for (int refresh = 1; refresh <= FRAME_COUNT; ++refresh) {
        while (ORIGIN + program_duration * next_program_frame + RENDER_TIME <= previous_presentation) {
            queue.push(make_frame(ORIGIN + program_duration * next_program_frame, next_program_frame));
            ++next_program_frame;
        }
        const auto scheduled = clock.predicted_completion();
        const auto target    = timeline.program_target(scheduled);
        const auto selection = queue.select_nearest(target);
        ASSERT_NE(selection.frame, nullptr);
        const auto advance = selection.frame->payload - previous_frame;
        EXPECT_GE(advance, 0);
        EXPECT_LE(advance, 2);
        if (program_duration > display_duration) {
            EXPECT_LE(advance, 1) << "unexpected skip at " << refresh;
            EXPECT_FALSE(advance == 0 && previous_advance == 0) << "repeat burst at " << refresh;
        } else {
            EXPECT_GT(advance, 0) << "unexpected repeat at " << refresh;
            EXPECT_FALSE(advance == 2 && previous_advance == 2) << "drop burst at " << refresh;
        }
        previous_frame   = selection.frame->payload;
        previous_advance = advance;

        const auto jitter = notification_jitter * (refresh % 17);
        const auto actual = initial_presentation + display_duration * refresh + jitter;
        // Feedback is deliberately supplied on EVERY refresh, including repeats.
        timeline.observe(scheduled, actual);
        clock.observe_completion(actual);
        previous_presentation = actual;
    }

    EXPECT_EQ(queue.metrics().overflow_drops, 0);
    EXPECT_EQ(queue.metrics().missing, 0);
    EXPECT_NEAR(
        utils::to_seconds(timeline.buffered_latency()), utils::to_seconds(initial_presentation - ORIGIN), 0.001);
    const auto expected_program_frame = (display_duration * FRAME_COUNT + program_duration / 2) / program_duration;
    EXPECT_NEAR(previous_frame, expected_program_frame, 1);
}

TEST_P(ScreenCadence, ContinuousFeedbackKeepsLiveBoundedQueueStable)
{
    const auto [program_duration, display_duration] = GetParam();
    verify_screen_cadence(program_duration, display_duration, utils::flicks{});
}

TEST_P(ScreenCadence, NotificationJitterDoesNotCreateExtraCadenceTransitions)
{
    const auto [program_duration, display_duration] = GetParam();
    verify_screen_cadence(program_duration, display_duration, utils::to_flicks(0.0001));
}

TEST_P(ScreenCadence, DelayedHardwareFeedbackKeepsScheduledOutputCadenceStable)
{
    const auto [program_duration, output_duration] = GetParam();
    constexpr auto                   ORIGIN        = utils::to_flicks(100.0);
    constexpr int                    PREROLL       = 4;
    media::timed_output_queue_s<int> queue({.capacity = 8});
    media::playout_timeline_s        timeline;
    timeline.initialize(ORIGIN + program_duration * PREROLL, ORIGIN);
    std::deque<std::pair<utils::flicks, utils::flicks>> feedback;
    int                                                 next_frame     = 0;
    int                                                 previous_frame = -1;
    for (int interval = 0; interval < 6'000; ++interval) {
        const auto scheduled = ORIGIN + program_duration * PREROLL + output_duration * interval;
        while (ORIGIN + program_duration * next_frame < scheduled) {
            queue.push(make_frame(ORIGIN + program_duration * next_frame, next_frame));
            ++next_frame;
        }
        const auto target   = timeline.program_target(scheduled);
        const auto selected = queue.select_nearest(target);
        ASSERT_NE(selected.frame, nullptr);
        EXPECT_GE(selected.frame->payload, previous_frame);
        EXPECT_LE(std::chrono::abs(selected.frame->program_target_time - target), program_duration / 2);
        previous_frame = selected.frame->payload;
        // Hardware completion arrives after several later frames were scheduled.
        // Feed back the prediction saved for this frame, not today's prediction.
        feedback.emplace_back(scheduled, scheduled + utils::to_flicks(0.00005) * (interval % 7));
        if (feedback.size() > PREROLL) {
            timeline.observe(feedback.front().first, feedback.front().second);
            feedback.pop_front();
        }
    }
    EXPECT_EQ(queue.metrics().overflow_drops, 0);
    EXPECT_EQ(queue.metrics().missing, 0);
}

INSTANTIATE_TEST_SUITE_P(RateMismatch,
                         ScreenCadence,
                         testing::Values(std::pair{utils::flicks{11'760'000}, utils::flicks{11'771'760}},
                                         std::pair{utils::flicks{11'771'760}, utils::flicks{11'760'000}},
                                         std::pair{utils::flicks{14'112'000}, utils::flicks{11'760'000}},
                                         std::pair{utils::flicks{23'520'000}, utils::flicks{11'760'000}},
                                         std::pair{utils::flicks{11'760'000}, utils::flicks{11'760'000}}));

TEST(ScreenTimeline, ContinuousLatenessCorrectionDoesNotAccumulateOrDependOnSelectedPts)
{
    constexpr auto            LATENCY  = utils::to_flicks(0.04);
    constexpr auto            DURATION = utils::flicks{11'760'000};
    constexpr auto            DELAY    = utils::to_flicks(0.002);
    media::playout_timeline_s timeline;
    timeline.initialize(LATENCY, utils::flicks{});
    for (int refresh = 1; refresh <= 1'000; ++refresh) {
        const auto scheduled = LATENCY + DURATION * refresh;
        timeline.observe(scheduled, scheduled + DELAY);
        EXPECT_LE(timeline.program_target(scheduled), scheduled + DELAY - LATENCY);
        if (refresh > 900) {
            EXPECT_NEAR(utils::to_seconds(timeline.program_target(scheduled) - scheduled + LATENCY),
                        utils::to_seconds(DELAY),
                        0.00001);
        }
    }
    for (int refresh = 1; refresh <= 1'000; ++refresh) {
        const auto scheduled = LATENCY + DURATION * (1'000 + refresh);
        timeline.observe(scheduled, scheduled);
    }
    EXPECT_EQ(timeline.buffered_latency(), LATENCY);
    EXPECT_NEAR(utils::to_seconds(timeline.program_target(LATENCY)), 0.0, 0.00001);
}

TEST(PresentationClock, DelayedNotificationDoesNotAddAnExtraIntervalToTheClock)
{
    constexpr auto              DURATION = utils::flicks{11'760'000};
    constexpr auto              ORIGIN   = utils::to_flicks(100.0);
    media::presentation_clock_s clock(media::presentation_clock_mode_e::observed, DURATION, ORIGIN);
    for (int refresh = 1; refresh <= 600; ++refresh) {
        // A notification arrives 10 ms late; the following refresh is on time.
        // Counting only consecutive arrival deltas would insert an extra tick.
        const auto delay  = refresh % 120 == 1 ? utils::to_flicks(0.010) : utils::flicks{};
        const auto result = clock.observe_completion(ORIGIN + DURATION * refresh + delay);
        if (refresh % 120 == 0) {
            EXPECT_NEAR(utils::to_seconds(clock.predicted_completion() - ORIGIN),
                        utils::to_seconds(DURATION * (refresh + 1)),
                        0.001);
            EXPECT_NEAR(result.refresh_hz, 60.0, 0.01);
        }
    }
}

TEST(PresentationClock, WakeupJitterAndAMissedRefreshKeepContinuousFeedbackBounded)
{
    constexpr auto              DURATION = utils::flicks{11'760'000};
    constexpr auto              ORIGIN   = utils::to_flicks(100.0);
    constexpr auto              LATENCY  = DURATION * 2;
    media::presentation_clock_s clock(media::presentation_clock_mode_e::observed, DURATION, ORIGIN);
    media::playout_timeline_s   timeline;
    timeline.initialize(ORIGIN, ORIGIN - LATENCY);
    for (int refresh = 1; refresh <= 3'000; ++refresh) {
        if (refresh == 300) {
            continue; // One genuine missed display refresh.
        }
        const auto scheduled = clock.predicted_completion();
        const auto jitter    = utils::to_flicks(refresh % 2 == 0 ? 0.0001 : 0.002);
        const auto actual    = ORIGIN + DURATION * refresh + jitter;
        timeline.observe(scheduled, actual);
        clock.observe_completion(actual);
        // The missed refresh itself cannot be predicted. The next selection
        // must immediately follow the recovered display interval instead of
        // replaying the missed slot or retaining its error as permanent latency.
        const auto next_target = timeline.program_target(clock.predicted_completion());
        EXPECT_LT(std::chrono::abs(next_target - (ORIGIN + DURATION * (refresh + 1) - LATENCY)), DURATION / 2)
            << "refresh " << refresh;
    }
    EXPECT_EQ(timeline.buffered_latency(), LATENCY);
}

} // namespace
