#include "nodes/ffmpeg/detail/playback_timeline.hpp"

#include <gtest/gtest.h>

namespace miximus::nodes::ffmpeg::detail { namespace {

TEST(PlaybackTimeline, PauseResumeDoesNotCatchUpInactiveTime)
{
    playback_timeline_s clock;
    clock.tick(utils::flicks{100}, 1, true);
    clock.tick(utils::flicks{110}, 1, true);
    EXPECT_EQ(clock.position().count(), 10);
    clock.tick(utils::flicks{120}, 1, false);
    clock.tick(utils::flicks{1000}, 1, true);
    EXPECT_EQ(clock.position().count(), 10);
    clock.tick(utils::flicks{1010}, 1, true);
    EXPECT_EQ(clock.position().count(), 20);
}

TEST(PlaybackTimeline, ProgramEpochReanchorsWithoutRewindingMedia)
{
    playback_timeline_s clock;
    clock.tick(utils::flicks{100}, 1, true);
    clock.tick(utils::flicks{140}, 1, true);
    clock.tick(utils::flicks{0}, 2, true);
    EXPECT_EQ(clock.position().count(), 40);
    clock.tick(utils::flicks{10}, 2, true);
    EXPECT_EQ(clock.position().count(), 50);
    clock.rewind();
    clock.tick(utils::flicks{1000}, 2, true);
    EXPECT_EQ(clock.position().count(), 0);
}

TEST(PlaybackTimeline, SkippedEvaluationsAdvanceTheMediaClock)
{
    playback_timeline_s clock;
    clock.tick(utils::flicks{100}, 1, true);
    clock.tick(utils::flicks{500}, 1, true);
    EXPECT_EQ(clock.position().count(), 400);
}

}} // namespace miximus::nodes::ffmpeg::detail
