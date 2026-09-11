#pragma once

#include "utils/flicks.hpp"

#include <algorithm>
#include <cstdint>

namespace miximus::media {

struct presentation_clock_result_s
{
    utils::flicks::rep interval_count{1};
    utils::flicks      completion_interval{};
    double             refresh_hz{};
};

enum class presentation_clock_mode_e : uint8_t
{
    nominal,
    observed,
};

// Present-wait observations include host wake-up jitter. Recover display phase
// independently of source PTS, and do not count a delayed notification followed
// by a short interval as two full refreshes. The nominal mode is a fallback for
// devices without display completion feedback.
class presentation_clock_s
{
    static constexpr utils::flicks::rep RATE_OBSERVATION_INTERVALS = 120;
    static constexpr utils::flicks::rep PHASE_FILTER_DIVISOR       = 128;
    static constexpr utils::flicks::rep RATE_FILTER_DIVISOR        = 8;

    presentation_clock_mode_e mode_;
    utils::flicks             nominal_frame_duration_;
    utils::flicks             frame_duration_;
    utils::flicks             previous_completion_;
    utils::flicks             phase_;
    utils::flicks             rate_reference_;
    utils::flicks::rep        rate_intervals_{};

  public:
    presentation_clock_s(presentation_clock_mode_e mode,
                         utils::flicks             nominal_frame_duration,
                         utils::flicks             initial_completion)
        : mode_(mode)
        , nominal_frame_duration_(nominal_frame_duration)
        , frame_duration_(nominal_frame_duration)
        , previous_completion_(initial_completion)
        , phase_(initial_completion)
        , rate_reference_(initial_completion)
    {
    }

    utils::flicks predicted_completion() const noexcept { return phase_ + frame_duration_; }

    presentation_clock_result_s observe_completion(utils::flicks completion)
    {
        const auto completion_interval = completion - previous_completion_;
        const auto minimum_intervals   = mode_ == presentation_clock_mode_e::nominal ? 1 : 0;
        const auto interval_count      = std::max<utils::flicks::rep>(
            minimum_intervals, (completion - phase_ + frame_duration_ / 2) / frame_duration_);
        phase_ += frame_duration_ * interval_count;

        if (mode_ == presentation_clock_mode_e::observed) {
            phase_ += (completion - phase_) / PHASE_FILTER_DIVISOR;
            rate_intervals_ += interval_count;
            if (rate_intervals_ >= RATE_OBSERVATION_INTERVALS) {
                const auto observed_duration = (completion - rate_reference_) / rate_intervals_;
                // GLFW reports integer Hz. Permit the rounding error at low
                // refresh rates, while rejecting a pause as a new display rate.
                const auto rounding_allowance = nominal_frame_duration_ / 50;
                const auto bounded_duration   = std::clamp(observed_duration,
                                                         nominal_frame_duration_ - rounding_allowance,
                                                         nominal_frame_duration_ + rounding_allowance);
                frame_duration_ += (bounded_duration - frame_duration_) / RATE_FILTER_DIVISOR;
                rate_reference_ = completion;
                rate_intervals_ = 0;
            }
        }
        previous_completion_ = completion;

        return {
            .interval_count      = interval_count,
            .completion_interval = completion_interval,
            .refresh_hz          = 1.0 / utils::to_seconds(frame_duration_),
        };
    }
};

} // namespace miximus::media
