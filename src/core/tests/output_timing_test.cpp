#include "media/output_buffer_watermark.hpp"
#include "media/output_runtime_metrics.hpp"
#include "media/timed_output_queue.hpp"
#include "utils/flicks.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace {
using namespace miximus;

media::output_frame_s<int> make_frame(uint64_t sequence, utils::flicks pts, int value, uint64_t epoch = 1)
{
    return {
        .id =
            {
                 .epoch    = epoch,
                 .sequence = sequence,
                 .pts      = pts,
                 .duration = utils::to_flicks(1.0 / 60.0),
                 },
        .value = value,
    };
}

TEST(OutputBufferWatermark, RefillsFromPrerollToTargetAndAfterCompletions)
{
    media::output_buffer_watermark_s watermark(4);
    watermark.frame_scheduled();
    watermark.frame_scheduled();
    EXPECT_EQ(watermark.refill_count(), 2);

    watermark.frame_scheduled();
    watermark.frame_scheduled();
    EXPECT_EQ(watermark.refill_count(), 0);
    EXPECT_TRUE(watermark.frame_completed());
    EXPECT_EQ(watermark.refill_count(), 1);
}

TEST(OutputBufferWatermark, RejectsInvalidTargetsAndUnexpectedCompletions)
{
    EXPECT_THROW((void)media::output_buffer_watermark_s(0), std::invalid_argument);

    media::output_buffer_watermark_s watermark(1);
    EXPECT_FALSE(watermark.frame_completed());
}

TEST(OutputBufferWatermark, PrerollAboveTheSteadyTargetDrainsNaturally)
{
    media::output_buffer_watermark_s watermark(2);
    for (size_t i = 0; i < 4; ++i) {
        watermark.frame_scheduled();
    }

    EXPECT_TRUE(watermark.frame_completed());
    EXPECT_EQ(watermark.refill_count(), 0);
    EXPECT_TRUE(watermark.frame_completed());
    EXPECT_EQ(watermark.refill_count(), 0);
    EXPECT_TRUE(watermark.frame_completed());
    EXPECT_EQ(watermark.refill_count(), 1);
}

TEST(TimedOutputQueue, SelectsNewestEligibleFrameAndRepeatsIt)
{
    media::timed_output_queue_s<int> queue;
    queue.push(make_frame(1, utils::to_flicks(0.00), 10));
    queue.push(make_frame(2, utils::to_flicks(0.01), 20));
    queue.push(make_frame(3, utils::to_flicks(0.02), 30));

    const auto selected = queue.select(1, utils::to_flicks(0.015));
    ASSERT_EQ(selected.selection, media::output_frame_selection_e::new_frame);
    ASSERT_NE(selected.frame, nullptr);
    EXPECT_EQ(selected.frame->value, 20);

    const auto repeat = queue.select(1, utils::to_flicks(0.016));
    ASSERT_EQ(repeat.selection, media::output_frame_selection_e::repeat);
    ASSERT_NE(repeat.frame, nullptr);
    EXPECT_EQ(repeat.frame->value, 20);

    EXPECT_EQ(queue.metrics().selection_drops, 1);
    EXPECT_EQ(queue.metrics().repeated, 1);
}

TEST(TimedOutputQueue, ConvertsSlowerProgramCadenceWithExplicitRepeats)
{
    constexpr auto SOURCE_DURATION = utils::to_flicks(1.0 / 50.0);
    constexpr auto OUTPUT_DURATION = utils::to_flicks(1.0 / 60.0);

    media::timed_output_queue_s<int> queue({.capacity = 64, .early_tolerance = SOURCE_DURATION / 2});
    for (uint64_t frame = 0; frame < 50; ++frame) {
        queue.push(
            make_frame(frame, SOURCE_DURATION * static_cast<utils::flicks::rep>(frame), static_cast<int>(frame)));
    }

    uint64_t new_frames{};
    uint64_t repeats{};
    for (uint64_t slot = 0; slot < 60; ++slot) {
        const auto selection = queue.select(1, OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot));
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
        queue.push(
            make_frame(frame, SOURCE_DURATION * static_cast<utils::flicks::rep>(frame), static_cast<int>(frame)));
    }

    for (uint64_t slot = 0; slot < 50; ++slot) {
        EXPECT_EQ(queue.select(1, OUTPUT_DURATION * static_cast<utils::flicks::rep>(slot)).selection,
                  media::output_frame_selection_e::new_frame);
    }
    EXPECT_EQ(queue.metrics().selection_drops, 10);
}

TEST(TimedOutputQueue, ClearsRetainedFrameAcrossEpochChanges)
{
    media::timed_output_queue_s<int> queue;
    queue.push(make_frame(1, {}, 10));
    ASSERT_EQ(queue.select(1, {}).selection, media::output_frame_selection_e::new_frame);

    const auto selection = queue.select(2, {});
    EXPECT_EQ(selection.selection, media::output_frame_selection_e::missing);
    EXPECT_EQ(selection.frame, nullptr);
    EXPECT_EQ(queue.metrics().discontinuities, 1);
}

TEST(TimedOutputQueue, DiscardsLateCompletionWithoutRegressingTheOutput)
{
    media::timed_output_queue_s<int> queue;
    queue.push(make_frame(2, utils::to_flicks(0.02), 20));
    ASSERT_EQ(queue.select(1, utils::to_flicks(0.02)).selection, media::output_frame_selection_e::new_frame);

    queue.push(make_frame(1, utils::to_flicks(0.01), 10));
    const auto selection = queue.select(1, utils::to_flicks(0.03));
    ASSERT_EQ(selection.selection, media::output_frame_selection_e::repeat);
    ASSERT_NE(selection.frame, nullptr);
    EXPECT_EQ(selection.frame->value, 20);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
}

TEST(TimedOutputQueue, DropsOldestQueuedFramesAtCapacity)
{
    media::timed_output_queue_s<int> queue({.capacity = 2});
    queue.push(make_frame(1, utils::to_flicks(0.01), 10));
    queue.push(make_frame(2, utils::to_flicks(0.02), 20));
    queue.push(make_frame(3, utils::to_flicks(0.03), 30));

    const auto selection = queue.select(1, utils::to_flicks(0.03));
    ASSERT_NE(selection.frame, nullptr);
    EXPECT_EQ(selection.frame->value, 30);
    EXPECT_EQ(queue.metrics().overflow_drops, 1);
    EXPECT_EQ(queue.metrics().selection_drops, 1);
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
    const auto                      start = std::chrono::steady_clock::time_point{};

    metrics.observe_completion(start, 10);
    metrics.observe_completion(start + std::chrono::milliseconds(20), 11);
    metrics.observe_completion(start + std::chrono::milliseconds(35), 12);
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
    EXPECT_EQ(snapshot.completion_interval_max_sequence, 11);
    EXPECT_EQ(snapshot.output_queue_depth, 1);
    EXPECT_EQ(snapshot.output_queue_depth_max, 5);
    EXPECT_EQ(snapshot.buffered_frames_min, 0);
    EXPECT_EQ(snapshot.buffered_frames_max, 4);
    EXPECT_EQ(snapshot.buffered_below_target_samples, 2);
    EXPECT_EQ(snapshot.buffered_zero_samples, 1);
    EXPECT_EQ(snapshot.refill_shortfalls, 1);
}

} // namespace
