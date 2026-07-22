#pragma once
#include "core/frame_context.hpp"
#include "types/frame_rate.hpp"
#include "utils/flicks.hpp"

#include <cstdint>
#include <string_view>

namespace miximus::core {

class clock_source_i;

struct late_frame_policy_s
{
    uint32_t allowed_lateness_frames{1};

    uint64_t frames_to_skip(utils::flicks now, utils::flicks next_target, utils::flicks duration) const;
};

struct frame_scheduler_metrics_s
{
    uint64_t      frame_number{};
    uint64_t      epoch{};
    utils::flicks pts{};
    utils::flicks render_start{};
    utils::flicks render_end{};
    utils::flicks render_duration{};
    utils::flicks start_lateness{};
    utils::flicks deadline_margin{};
    uint64_t      skipped_frames{};
    uint64_t      skipped_frames_total{};
    bool          sustained_overload{};
};

class frame_scheduler_s
{
    clock_source_i&     clock_;
    late_frame_policy_s late_policy_;

    frame_rate_s  frame_rate_{};
    utils::flicks duration_{};
    utils::flicks anchor_time_{};
    uint64_t      timeline_frame_{};
    uint64_t      next_frame_number_{};
    uint64_t      epoch_{};
    uint64_t      skipped_frames_total_{};
    utils::flicks accumulated_overload_{};
    bool          initialized_{};
    bool          frame_active_{};

    frame_context_s           context_{};
    frame_scheduler_metrics_s metrics_{};

  public:
    explicit frame_scheduler_s(clock_source_i& clock, late_frame_policy_s late_policy = {});

    frame_context_s                  begin_frame(frame_rate_s frame_rate, uint64_t settings_revision);
    const frame_scheduler_metrics_s& finish_frame();

    const frame_scheduler_metrics_s& metrics() const { return metrics_; }
    std::string_view                 clock_name() const;
};

} // namespace miximus::core
