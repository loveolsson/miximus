#include "frame_scheduler.hpp"

#include "core/clock_source.hpp"

#include <algorithm>
#include <stdexcept>

namespace miximus::core {

uint64_t late_frame_policy_s::frames_to_skip(utils::flicks now, utils::flicks next_target, utils::flicks duration) const
{
    if (duration <= utils::k_flicks_zero_seconds) {
        throw std::invalid_argument("Late-frame policy requires a positive frame duration");
    }

    const auto permitted_lateness = duration * allowed_lateness_frames;
    if (now <= next_target + permitted_lateness) {
        return 0;
    }

    const auto excess = now - next_target - permitted_lateness;
    return static_cast<uint64_t>(((excess.count() - 1) / duration.count()) + 1);
}

frame_scheduler_s::frame_scheduler_s(clock_source_i& clock, late_frame_policy_s late_policy)
    : clock_(clock)
    , late_policy_(late_policy)
{
}

frame_context_s frame_scheduler_s::begin_frame(frame_rate_s frame_rate)
{
    if (frame_active_) {
        throw std::logic_error("Cannot begin a frame before finishing the previous frame");
    }

    bool discontinuity = false;
    if (!initialized_ || frame_rate != frame_rate_) {
        const auto duration = get_frame_duration(frame_rate);
        if (!duration.has_value()) {
            throw std::invalid_argument("Frame scheduler requires an exactly representable frame rate");
        }

        frame_rate_           = frame_rate;
        duration_             = *duration;
        anchor_time_          = clock_.now();
        timeline_frame_       = 0;
        accumulated_overload_ = utils::k_flicks_zero_seconds;
        initialized_          = true;
        discontinuity         = true;
        ++epoch_;
    }

    const auto frame_offset = duration_ * static_cast<utils::flicks::rep>(timeline_frame_);
    const auto target_time  = anchor_time_ + frame_offset;
    context_                = {
                       .frame_number    = next_frame_number_,
                       .epoch           = epoch_,
                       .pts             = frame_offset,
                       .duration        = duration_,
                       .target_time     = target_time,
                       .render_deadline = target_time + duration_,
                       .discontinuity   = discontinuity,
    };

    metrics_ = {
        .frame_number = context_.frame_number,
        .epoch        = context_.epoch,
        .pts          = context_.pts,
        .render_start = clock_.now(),
    };
    frame_active_ = true;
    return context_;
}

const frame_scheduler_metrics_s& frame_scheduler_s::finish_frame()
{
    if (!frame_active_) {
        throw std::logic_error("Cannot finish a frame before beginning it");
    }

    metrics_.render_end      = clock_.now();
    metrics_.render_duration = metrics_.render_end - metrics_.render_start;
    metrics_.start_lateness  = std::max(metrics_.render_start - context_.target_time, utils::k_flicks_zero_seconds);
    metrics_.deadline_margin = context_.render_deadline - metrics_.render_end;

    if (metrics_.deadline_margin < utils::k_flicks_zero_seconds) {
        accumulated_overload_ += duration_;
    } else {
        accumulated_overload_ = utils::k_flicks_zero_seconds;
    }
    metrics_.sustained_overload = accumulated_overload_ >= utils::k_flicks_one_second;

    const auto next_timeline_frame = timeline_frame_ + 1;
    const auto next_target         = anchor_time_ + duration_ * static_cast<utils::flicks::rep>(next_timeline_frame);
    metrics_.skipped_frames        = late_policy_.frames_to_skip(metrics_.render_end, next_target, duration_);
    skipped_frames_total_ += metrics_.skipped_frames;
    metrics_.skipped_frames_total = skipped_frames_total_;

    timeline_frame_ = next_timeline_frame + metrics_.skipped_frames;
    next_frame_number_ += 1 + metrics_.skipped_frames;
    frame_active_ = false;

    clock_.wait_until(anchor_time_ + duration_ * static_cast<utils::flicks::rep>(timeline_frame_));
    return metrics_;
}

std::string_view frame_scheduler_s::clock_name() const { return clock_.name(); }

} // namespace miximus::core
