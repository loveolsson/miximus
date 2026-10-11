import type { NodeStatusSection } from "./interfaces";

export const sourceTimingSection: NodeStatusSection = {
  title: "Timing",
  fields: [
    { key: "source_recovered_rate", label: "Recovered clock ratio", precision: 6 },
    { key: "source_observed_rate", label: "Observed clock ratio", precision: 6 },
    { key: "source_phase_offset_us", label: "Phase offset (µs)", format: "integer" },
    { key: "source_phase_error_us", label: "Phase error (µs)", format: "integer" },
    { key: "source_phase_adjustment_us", label: "Phase adjustment (µs)", format: "integer" },
    {
      key: "source_repeat_next_frame_lead_min_us",
      label: "Repeat lead min (µs)",
      format: "integer",
    },
    {
      key: "source_repeat_next_frame_lead_max_us",
      label: "Repeat lead max (µs)",
      format: "integer",
    },
  ],
};

export const sourceQueueSection: NodeStatusSection = {
  title: "Timed queue",
  fields: [
    { key: "source_queue_pushed", label: "Timed queue input", format: "integer" },
    { key: "source_queue_depth", label: "Timed queue depth", format: "integer" },
    { key: "source_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
    { key: "source_queue_selection_drops", label: "Timing drops", format: "integer" },
    { key: "source_queue_repeated", label: "Timing repeats", format: "integer" },
    { key: "source_queue_starvation_repeats", label: "Starvation repeats", format: "integer" },
    { key: "source_queue_timing_repeats", label: "Early-frame repeats", format: "integer" },
    { key: "source_queue_missing", label: "Timing missing", format: "integer" },
    { key: "source_queue_discontinuities", label: "Discontinuities", format: "integer" },
  ],
};
