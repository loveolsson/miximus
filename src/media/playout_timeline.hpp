#pragma once

#include "presentation_timeline.hpp"

#include <stdexcept>

namespace miximus::media {

// Keep the preroll delay separate from output scheduling error. Selecting a
// discrete source PTS adds cadence quantization; feeding that back as latency
// would turn ordinary rate conversion into cumulative queue drift.
class playout_timeline_s
{
    static constexpr utils::flicks::rep PHASE_FILTER_DIVISOR = 128;

    std::optional<utils::flicks> buffered_latency_;
    presentation_timeline_s      scheduling_error_;
    utils::flicks                scheduling_adjustment_;

  public:
    void initialize(utils::flicks presentation, utils::flicks program_target) noexcept
    {
        buffered_latency_      = presentation - program_target;
        scheduling_error_      = {};
        scheduling_adjustment_ = {};
    }

    bool initialized() const noexcept { return buffered_latency_.has_value(); }

    utils::flicks buffered_latency() const
    {
        if (!buffered_latency_) {
            throw std::logic_error("playout timeline has no initial presentation");
        }
        return *buffered_latency_;
    }

    utils::flicks program_target(utils::flicks presentation) const
    {
        // If output presentation is consistently later than the prediction,
        // select for that later time. Subtracting the error would move selection
        // in the opposite direction and count the same lateness twice.
        return presentation + scheduling_adjustment_ - buffered_latency();
    }

    void observe(utils::flicks scheduled_presentation, utils::flicks actual_presentation) noexcept
    {
        // Observe repeats too: output timing is independent of whether this
        // interval needed a new source image. The scheduled time does not contain
        // the controller's previous latency correction or source quantization.
        const auto error = scheduling_error_.observe_latency(actual_presentation, scheduled_presentation);
        // Slew the phase correction. Even averaged host wake-up noise must not
        // toggle source-frame selection back and forth at a cadence boundary.
        scheduling_adjustment_ += (error - scheduling_adjustment_) / PHASE_FILTER_DIVISOR;
    }
};

} // namespace miximus::media
