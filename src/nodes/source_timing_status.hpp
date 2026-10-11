#pragma once

#include "media/timed_source_queue.hpp"
#include "types/node_status.hpp"

namespace miximus::nodes {

inline status::source_timing_status_s source_timing_status(const media::timed_source_queue_metrics_s& metrics)
{
    return {
        .source_queue_pushed                  = metrics.pushed,
        .source_queue_depth                   = metrics.queued,
        .source_queue_overflow_drops          = metrics.overflow_drops,
        .source_queue_selection_drops         = metrics.selection_drops,
        .source_queue_repeated                = metrics.repeated,
        .source_queue_starvation_repeats      = metrics.starvation_repeats,
        .source_queue_timing_repeats          = metrics.timing_repeats,
        .source_queue_missing                 = metrics.missing,
        .source_queue_discontinuities         = metrics.discontinuities,
        .source_queue_transfer_failures       = metrics.transfer_failures,
        .source_queue_transfer_cancellations  = metrics.transfer_cancellations,
        .source_recovered_rate                = metrics.recovered_rate,
        .source_observed_rate                 = metrics.observed_rate,
        .source_phase_offset_us               = metrics.phase_offset,
        .source_phase_error_us                = metrics.phase_error,
        .source_phase_adjustment_us           = metrics.phase_adjustment,
        .source_repeat_next_frame_lead_min_us = metrics.repeat_next_frame_lead_min,
        .source_repeat_next_frame_lead_max_us = metrics.repeat_next_frame_lead_max,
    };
}

} // namespace miximus::nodes
