import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { node_type_e } from "./node_type";
import {
  DropdownInterface,
  StatusDropdownInterface,
  NodeStatusInterface,
  type NodeStatusSection,
} from "./interfaces";

const decklinkInputStatus: readonly NodeStatusSection[] = [
  {
    title: "Input",
    fields: [
      { key: "connected", label: "Connection" },
      { key: "active_format", label: "Active format" },
      { key: "signal_locked", label: "Input signal", format: "locked" },
      { key: "capture_busy", label: "Capture", format: "busy" },
      { key: "ancillary_signal_locked", label: "Ancillary signal", format: "locked" },
    ],
  },
  {
    title: "Detected signal",
    fields: [
      { key: "detected_format", label: "Format" },
      { key: "detected_colorspace", label: "Color space" },
      { key: "detected_dynamic_range", label: "Dynamic range" },
      { key: "detected_field_dominance", label: "Field dominance" },
      { key: "detected_sdi_link_configuration", label: "SDI links" },
      { key: "input_pixel_format", label: "Pixel format" },
    ],
  },
  {
    title: "Device",
    fields: [
      { key: "temperature_c", label: "Temperature", format: "temperature" },
      { key: "pcie_link_width", label: "PCIe link width", format: "integer" },
      { key: "pcie_link_speed", label: "PCIe link speed", format: "integer" },
    ],
  },
  {
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
  },
  {
    title: "Capture",
    fields: [
      { key: "frames_received", label: "Received", format: "integer" },
      { key: "frames_missing", label: "Missing", format: "integer" },
      { key: "no_input_source_frames", label: "No source", format: "integer" },
      { key: "available_video_frames", label: "Available", format: "integer" },
      { key: "content_frames_sampled", label: "Content samples", format: "integer" },
      { key: "content_frame_repeats", label: "Content repeats", format: "integer" },
      { key: "content_repeat_streak", label: "Current repeat streak", format: "integer" },
      { key: "content_repeat_streak_max", label: "Longest repeat streak", format: "integer" },
    ],
  },
  {
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
  },
  {
    title: "Transfers",
    fields: [
      { key: "upload_slot_drops", label: "Upload slot drops", format: "integer" },
      { key: "upload_acquire_slow_count", label: "Slow acquisitions", format: "integer" },
      { key: "upload_acquire_failures", label: "Acquisition failures", format: "integer" },
      { key: "upload_acquire_wait_max_us", label: "Longest acquisition (µs)", format: "integer" },
      { key: "source_queue_transfer_failures", label: "Transfer failures", format: "integer" },
      {
        key: "source_queue_transfer_cancellations",
        label: "Transfer cancellations",
        format: "integer",
      },
    ],
  },
];

const decklinkOutputStatus: readonly NodeStatusSection[] = [
  {
    title: "Output",
    fields: [
      { key: "connected", label: "Connection" },
      { key: "active_format", label: "Active format" },
      { key: "playback_busy", label: "Playback", format: "active" },
      { key: "output_pixel_format", label: "Pixel format" },
      { key: "requested_keyer_mode", label: "Requested keyer" },
      { key: "active_keyer_mode", label: "Active keyer" },
      { key: "keyer_fallback_reason", label: "Keyer fallback" },
      { key: "reference_locked", label: "Reference signal", format: "locked" },
      { key: "reference_format", label: "Reference format" },
    ],
  },
  {
    title: "Device",
    fields: [
      { key: "temperature_c", label: "Temperature", format: "temperature" },
      { key: "pcie_link_width", label: "PCIe link width", format: "integer" },
      { key: "pcie_link_speed", label: "PCIe link speed", format: "integer" },
    ],
  },
  {
    title: "Device playback",
    fields: [
      { key: "frames_completed", label: "Completed", format: "integer" },
      { key: "frames_displayed_late", label: "Displayed late", format: "integer" },
      { key: "frames_dropped", label: "Dropped", format: "integer" },
      { key: "frames_flushed", label: "Flushed", format: "integer" },
      { key: "buffered_video_frames", label: "Buffered", format: "integer" },
      { key: "buffered_video_frames_min", label: "Minimum buffered", format: "integer" },
      { key: "buffered_video_frames_max", label: "Maximum buffered", format: "integer" },
      { key: "buffered_below_target_samples", label: "Below-target samples", format: "integer" },
      { key: "buffered_zero_samples", label: "Empty-buffer samples", format: "integer" },
      { key: "output_refill_shortfalls", label: "Refill shortfalls", format: "integer" },
    ],
  },
  {
    title: "Output timing",
    fields: [
      { key: "output_latency_us", label: "Output latency (µs)", format: "integer" },
      { key: "program_selection_offset_us", label: "Selection offset (µs)", format: "integer" },
      { key: "completion_intervals", label: "Clock samples", format: "integer" },
      { key: "completion_interval_max_us", label: "Longest interval (µs)", format: "integer" },
      { key: "completion_time_failures", label: "Clock failures", format: "integer" },
    ],
  },
  {
    title: "Program queue",
    fields: [
      { key: "program_frames_received", label: "Program frames", format: "integer" },
      { key: "program_queue_depth", label: "Queue depth", format: "integer" },
      { key: "program_queue_depth_max", label: "Maximum queue depth", format: "integer" },
      { key: "program_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
      { key: "program_timing_drops", label: "Timing drops", format: "integer" },
      { key: "program_frames_repeated", label: "Total repeats", format: "integer" },
      { key: "program_cadence_repeats", label: "Cadence repeats", format: "integer" },
      { key: "program_starvation_repeats", label: "Starvation repeats", format: "integer" },
      {
        key: "program_starvation_repeat_streak",
        label: "Current starvation streak",
        format: "integer",
      },
      {
        key: "program_starvation_repeat_streak_max",
        label: "Longest starvation streak",
        format: "integer",
      },
      { key: "program_frames_missing", label: "Timing missing", format: "integer" },
    ],
  },
  {
    title: "Transfers",
    fields: [
      { key: "render_target_drops", label: "Render target drops", format: "integer" },
      { key: "download_slots", label: "Download slots", format: "integer" },
      { key: "download_slots_free", label: "Free", format: "integer" },
      { key: "download_slots_rendering", label: "Rendering", format: "integer" },
      { key: "download_slots_queued", label: "Queued", format: "integer" },
      { key: "download_slots_ready", label: "Ready", format: "integer" },
      { key: "download_slots_cpu_reading", label: "CPU reading", format: "integer" },
      { key: "download_pending_allocations", label: "Pending allocations", format: "integer" },
      { key: "download_acquire_misses", label: "Acquisition misses", format: "integer" },
      { key: "download_transfers_completed", label: "Completed transfers", format: "integer" },
      { key: "download_transfer_failures", label: "Transfer failures", format: "integer" },
      {
        key: "download_transfer_duration_max_us",
        label: "Longest transfer (µs)",
        format: "integer",
      },
      { key: "download_allocation_failed", label: "Allocation", format: "failure" },
    ],
  },
  {
    title: "Content",
    fields: [
      { key: "content_frames_sampled", label: "Samples", format: "integer" },
      { key: "content_frame_repeats", label: "Repeated samples", format: "integer" },
      { key: "content_repeat_streak", label: "Current repeat streak", format: "integer" },
      { key: "content_repeat_streak_max", label: "Longest repeat streak", format: "integer" },
    ],
  },
];

export const DeckLinkInputNode = defineNode({
  type: node_type_e.decklink_input,
  title: "DeckLink Input",
  inputs: {
    status: () => new NodeStatusInterface(decklinkInputStatus),
    enabled: () => new CheckboxInterface("Enabled", false).setPort(false),
    device_name: () => new StatusDropdownInterface("Device", "device_names"),
  },
  outputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});

export const DeckLinkOutputNode = defineNode({
  type: node_type_e.decklink_output,
  title: "DeckLink Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(decklinkOutputStatus),
    enabled: () => new CheckboxInterface("Enabled", false).setPort(false),
    device_name: () => new StatusDropdownInterface("Device", "device_names"),
    display_mode: () => new StatusDropdownInterface("Display Mode", "display_modes"),
    keyer_mode: () =>
      new DropdownInterface("Keyer", "disabled", [
        { id: "disabled", label: "Disabled" },
        { id: "internal", label: "Internal" },
        { id: "external", label: "External" },
      ]),
  },
  outputs: {},
});
