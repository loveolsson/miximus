#include "media/media_clock.hpp"
#include "media/output_runtime_metrics.hpp"
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

    timeline.establish_latency(presentation, program);

    EXPECT_EQ(timeline.latency(), utils::to_flicks(0.1));
    EXPECT_EQ(timeline.map_presentation_to_program_target(presentation + utils::to_flicks(10.0)),
              program + utils::to_flicks(10.0));
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
    timeline.establish_latency(ORIGIN + utils::to_flicks(0.1), ORIGIN);
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
            timeline.establish_latency(presentation, PROGRAM_ORIGIN);
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

} // namespace
