#include "core/clock_source.hpp"
#include "core/frame_scheduler.hpp"
#include "types/frame_rate.hpp"
#include "utils/flicks.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

namespace {
using namespace miximus;

class fake_clock_source_s final : public core::clock_source_i
{
    utils::flicks time_;

  public:
    explicit fake_clock_source_s(utils::flicks time)
        : time_(time)
    {
    }

    utils::flicks now() const final { return time_; }

    void wait_until(utils::flicks time) final
    {
        if (time_ < time) {
            time_ = time;
        }
    }

    std::string_view name() const final { return "Fake"; }

    void advance(utils::flicks duration) { time_ += duration; }
    void set(utils::flicks time) { time_ = time; }
};

void expect_exact_timeline(frame_rate_s rate, uint64_t frame_count)
{
    constexpr utils::flicks START_TIME{987'654'321};

    fake_clock_source_s     clock(START_TIME);
    core::frame_scheduler_s scheduler(clock);
    const auto              duration = *get_frame_duration(rate);

    for (uint64_t frame = 0; frame < frame_count; ++frame) {
        const auto context = scheduler.begin_frame(rate, 7);
        const auto offset  = duration * static_cast<utils::flicks::rep>(frame);
        ASSERT_EQ(context.frame_number, frame) << "at frame " << frame;
        ASSERT_EQ(context.pts, offset) << "at frame " << frame;
        ASSERT_EQ(context.target_time, START_TIME + offset) << "at frame " << frame;
        ASSERT_EQ(context.settings_revision, 7) << "at frame " << frame;
        ASSERT_EQ(context.discontinuity, frame == 0) << "at frame " << frame;

        const auto& metrics = scheduler.finish_frame();
        ASSERT_EQ(metrics.skipped_frames, 0) << "at frame " << frame;
    }
}

TEST(FrameScheduler, IntegerTimelineDoesNotDrift) { expect_exact_timeline(DEFAULT_FRAME_RATE, 100'000); }

TEST(FrameScheduler, NtscTimelineDoesNotDrift)
{
    expect_exact_timeline({.numerator = 60'000, .denominator = 1'001}, 100'000);
}

TEST(FrameScheduler, RetainsUsefulLateFramesAndSkipsObsoleteFramesAtOnce)
{
    constexpr utils::flicks START_TIME{123'456};
    constexpr frame_rate_s  RATE{.numerator = 60, .denominator = 1};

    fake_clock_source_s     clock(START_TIME);
    core::frame_scheduler_s scheduler(clock);
    const auto              duration = *get_frame_duration(RATE);

    const auto first = scheduler.begin_frame(RATE, 1);
    clock.advance(duration + duration / 2);
    const auto& first_metrics = scheduler.finish_frame();
    EXPECT_EQ(first.frame_number, 0);
    EXPECT_EQ(first_metrics.skipped_frames, 0);

    const auto second = scheduler.begin_frame(RATE, 1);
    EXPECT_EQ(second.frame_number, 1);
    EXPECT_EQ(second.pts, duration);

    clock.set(START_TIME + duration * 5 + duration / 4);
    const auto& second_metrics = scheduler.finish_frame();
    EXPECT_EQ(second_metrics.skipped_frames, 3);
    EXPECT_EQ(second_metrics.skipped_frames_total, 3);

    const auto recovered = scheduler.begin_frame(RATE, 1);
    EXPECT_EQ(recovered.frame_number, 5);
    EXPECT_EQ(recovered.pts, duration * 5);
    EXPECT_EQ(recovered.target_time, START_TIME + duration * 5);
    scheduler.finish_frame();
}

TEST(FrameScheduler, RateChangeStartsANewEpochWithoutResettingFrameNumbers)
{
    constexpr utils::flicks START_TIME{444'000};

    fake_clock_source_s     clock(START_TIME);
    core::frame_scheduler_s scheduler(clock);

    const auto first = scheduler.begin_frame(DEFAULT_FRAME_RATE, 1);
    scheduler.finish_frame();
    const auto second = scheduler.begin_frame({.numerator = 50, .denominator = 1}, 2);

    EXPECT_EQ(first.epoch, 1);
    EXPECT_EQ(second.epoch, 2);
    EXPECT_TRUE(second.discontinuity);
    EXPECT_EQ(second.pts, utils::k_flicks_zero_seconds);
    EXPECT_EQ(second.frame_number, 1);
    EXPECT_EQ(second.target_time, clock.now());
    scheduler.finish_frame();
}

TEST(FrameScheduler, RejectsInvalidLifecycleAndFrameRates)
{
    fake_clock_source_s     clock(utils::k_flicks_zero_seconds);
    core::frame_scheduler_s scheduler(clock);

    EXPECT_THROW(scheduler.finish_frame(), std::logic_error);
    scheduler.begin_frame(DEFAULT_FRAME_RATE, 1);
    EXPECT_THROW(scheduler.begin_frame(DEFAULT_FRAME_RATE, 1), std::logic_error);
    scheduler.finish_frame();
    EXPECT_THROW(scheduler.begin_frame({.numerator = 59, .denominator = 1}, 1), std::invalid_argument);
}

TEST(FrameScheduler, ReportsSustainedOverloadAndClearsItAfterRecovery)
{
    constexpr utils::flicks START_TIME{777'000};
    constexpr frame_rate_s  RATE{.numerator = 60, .denominator = 1};

    fake_clock_source_s     clock(START_TIME);
    core::frame_scheduler_s scheduler(clock);

    for (uint32_t frame = 0; frame < 60; ++frame) {
        const auto context = scheduler.begin_frame(RATE, 1);
        clock.set(context.render_deadline + utils::flicks{1});
        scheduler.finish_frame();
    }
    EXPECT_TRUE(scheduler.metrics().sustained_overload);

    scheduler.begin_frame(RATE, 1);
    scheduler.finish_frame();
    EXPECT_FALSE(scheduler.metrics().sustained_overload);
}

uint64_t replay_trace(core::late_frame_policy_s policy, const std::vector<utils::flicks>& render_durations)
{
    fake_clock_source_s     clock(utils::k_flicks_zero_seconds);
    core::frame_scheduler_s scheduler(clock, policy);

    for (const auto render_duration : render_durations) {
        scheduler.begin_frame(DEFAULT_FRAME_RATE, 1);
        clock.advance(render_duration);
        scheduler.finish_frame();
    }
    return scheduler.metrics().skipped_frames_total;
}

TEST(FrameScheduler, RecordedWorkloadCanCompareLateFramePolicies)
{
    const auto        duration = *get_frame_duration(DEFAULT_FRAME_RATE);
    const std::vector trace{
        duration / 2,
        duration + duration / 2,
        duration / 4,
        duration * 3,
        duration / 2,
    };

    const auto strict_skips      = replay_trace(core::late_frame_policy_s{.allowed_lateness_frames = 0}, trace);
    const auto provisional_skips = replay_trace(core::late_frame_policy_s{.allowed_lateness_frames = 1}, trace);
    EXPECT_GT(strict_skips, provisional_skips);
}
} // namespace
