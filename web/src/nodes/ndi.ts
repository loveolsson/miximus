import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { node_type_e } from "./node_type";
import {
  FocusTrackingStringInterface,
  StatusDropdownInterface,
  NodeStatusInterface,
  type NodeStatusSection,
} from "./interfaces";

const ndiInputStatus: readonly NodeStatusSection[] = [
  {
    title: "Input",
    fields: [{ key: "connected", label: "Connection" }],
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
    title: "Receiver",
    fields: [
      { key: "frames_received", label: "Received", format: "integer" },
      { key: "invalid_frames", label: "Invalid", format: "integer" },
      { key: "receiver_video_drops", label: "Receiver drops", format: "integer" },
      { key: "receiver_queue_depth", label: "Receiver queue", format: "integer" },
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
      { key: "source_queue_transfer_failures", label: "Transfer failures", format: "integer" },
      {
        key: "source_queue_transfer_cancellations",
        label: "Transfer cancellations",
        format: "integer",
      },
    ],
  },
];

const ndiOutputStatus: readonly NodeStatusSection[] = [
  {
    title: "Output",
    fields: [{ key: "connected", label: "Sender" }],
  },
  {
    title: "Output timing",
    fields: [
      { key: "frames_sent", label: "Sent", format: "integer" },
      { key: "queued_frames", label: "Buffered", format: "integer" },
      { key: "output_latency_us", label: "Output latency (µs)", format: "integer" },
      { key: "program_selection_offset_us", label: "Selection offset (µs)", format: "integer" },
      { key: "output_intervals_skipped", label: "Skipped output intervals", format: "integer" },
    ],
  },
  {
    title: "Program queue",
    fields: [
      { key: "program_frames_received", label: "Program frames", format: "integer" },
      { key: "program_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
      { key: "program_timing_drops", label: "Timing drops", format: "integer" },
      { key: "program_frames_repeated", label: "Timing repeats", format: "integer" },
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
];

export const NdiInputNode = defineNode({
  type: node_type_e.ndi_input,
  title: "NDI Input",
  inputs: {
    status: () => new NodeStatusInterface(ndiInputStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    source_name: () => new StatusDropdownInterface("Source", "source_names"),
  },
  outputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});

export const NdiOutputNode = defineNode({
  type: node_type_e.ndi_output,
  title: "NDI Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(ndiOutputStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    source_name: () => new FocusTrackingStringInterface("Sender Name", ""),
  },
  outputs: {},
});
