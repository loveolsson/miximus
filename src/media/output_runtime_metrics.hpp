#pragma once
#include "media/timed_output_queue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace miximus::media {

struct output_runtime_metrics_snapshot_s
{
    uint64_t      completion_intervals{};
    utils::flicks completion_interval_max{};
    uint64_t      cadence_repeats{};
    uint64_t      starvation_repeats{};
    uint64_t      starvation_repeat_streak{};
    uint64_t      starvation_repeat_streak_max{};
    uint64_t      refill_shortfalls{};
    size_t        output_queue_depth{};
    size_t        output_queue_depth_max{};
    uint32_t      buffered_frames{};
    uint32_t      buffered_frames_min{};
    uint32_t      buffered_frames_max{};
    uint64_t      buffered_below_target_samples{};
    uint64_t      buffered_zero_samples{};
};

class output_runtime_metrics_s
{
    std::optional<utils::flicks> last_completion_;
    uint64_t                     completion_intervals_{};
    utils::flicks                completion_interval_max_{};
    uint64_t                     cadence_repeats_{};
    uint64_t                     starvation_repeats_{};
    uint64_t                     starvation_repeat_streak_{};
    uint64_t                     starvation_repeat_streak_max_{};
    uint64_t                     refill_shortfalls_{};
    size_t                       output_queue_depth_{};
    size_t                       output_queue_depth_max_{};
    uint32_t                     buffered_frames_{};
    uint32_t                     buffered_frames_min_{};
    uint32_t                     buffered_frames_max_{};
    uint64_t                     buffered_below_target_samples_{};
    uint64_t                     buffered_zero_samples_{};
    bool                         buffered_frames_observed_{};

  public:
    void observe_completion(utils::flicks time) noexcept
    {
        if (last_completion_.has_value()) {
            const auto interval = time - *last_completion_;
            ++completion_intervals_;
            if (interval > completion_interval_max_) {
                completion_interval_max_ = interval;
            }
        }
        last_completion_ = time;
    }

    void observe_selection(output_frame_selection_e selection, bool has_queued_frames) noexcept
    {
        if (selection != output_frame_selection_e::repeat) {
            starvation_repeat_streak_ = 0;
            return;
        }

        if (has_queued_frames) {
            ++cadence_repeats_;
            starvation_repeat_streak_ = 0;
            return;
        }

        ++starvation_repeats_;
        ++starvation_repeat_streak_;
        starvation_repeat_streak_max_ = std::max(starvation_repeat_streak_max_, starvation_repeat_streak_);
    }

    void observe_refill(size_t requested, size_t scheduled) noexcept
    {
        if (scheduled < requested) {
            ++refill_shortfalls_;
        }
    }

    void observe_output_queue_depth(size_t depth) noexcept
    {
        output_queue_depth_     = depth;
        output_queue_depth_max_ = std::max(output_queue_depth_max_, depth);
    }

    void observe_buffered_frames(uint32_t frames, uint32_t target) noexcept
    {
        buffered_frames_ = frames;
        if (!buffered_frames_observed_) {
            buffered_frames_min_      = frames;
            buffered_frames_observed_ = true;
        } else {
            buffered_frames_min_ = std::min(buffered_frames_min_, frames);
        }
        buffered_frames_max_ = std::max(buffered_frames_max_, frames);
        buffered_below_target_samples_ += frames < target ? 1 : 0;
        buffered_zero_samples_ += frames == 0 ? 1 : 0;
    }

    output_runtime_metrics_snapshot_s snapshot() const noexcept
    {
        return {
            .completion_intervals          = completion_intervals_,
            .completion_interval_max       = completion_interval_max_,
            .cadence_repeats               = cadence_repeats_,
            .starvation_repeats            = starvation_repeats_,
            .starvation_repeat_streak      = starvation_repeat_streak_,
            .starvation_repeat_streak_max  = starvation_repeat_streak_max_,
            .refill_shortfalls             = refill_shortfalls_,
            .output_queue_depth            = output_queue_depth_,
            .output_queue_depth_max        = output_queue_depth_max_,
            .buffered_frames               = buffered_frames_,
            .buffered_frames_min           = buffered_frames_min_,
            .buffered_frames_max           = buffered_frames_max_,
            .buffered_below_target_samples = buffered_below_target_samples_,
            .buffered_zero_samples         = buffered_zero_samples_,
        };
    }
};

} // namespace miximus::media
