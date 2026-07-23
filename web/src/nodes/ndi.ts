import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
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
      { key: "source_recovered_rate", label: "Recovered clock ratio" },
      { key: "source_phase_offset_us", label: "Phase offset (µs)", format: "integer" },
    ],
  },
  {
    title: "Frame statistics",
    fields: [
      { key: "frames_received", label: "Received", format: "integer" },
      { key: "invalid_frames", label: "Invalid", format: "integer" },
      { key: "receiver_video_drops", label: "Receiver drops", format: "integer" },
      { key: "receiver_queue_depth", label: "Receiver queue", format: "integer" },
      { key: "upload_slot_drops", label: "Upload slot drops", format: "integer" },
      { key: "source_queue_pushed", label: "Timed queue input", format: "integer" },
      { key: "source_queue_depth", label: "Timed queue depth", format: "integer" },
      { key: "source_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
      { key: "source_queue_selection_drops", label: "Timing drops", format: "integer" },
      { key: "source_queue_repeated", label: "Timing repeats", format: "integer" },
      { key: "source_queue_missing", label: "Timing missing", format: "integer" },
      { key: "source_queue_discontinuities", label: "Discontinuities", format: "integer" },
      { key: "source_queue_transfer_failures", label: "Transfer failures", format: "integer" },
    ],
  },
];

const ndiOutputStatus: readonly NodeStatusSection[] = [
  {
    title: "Output",
    fields: [{ key: "connected", label: "Sender" }],
  },
  {
    title: "Frame statistics",
    fields: [
      { key: "frames_sent", label: "Sent", format: "integer" },
      { key: "queued_frames", label: "Buffered", format: "integer" },
      { key: "render_target_drops", label: "Render target drops", format: "integer" },
      { key: "program_frames_received", label: "Program frames", format: "integer" },
      { key: "program_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
      { key: "program_timing_drops", label: "Timing drops", format: "integer" },
      { key: "program_frames_repeated", label: "Timing repeats", format: "integer" },
      { key: "program_frames_missing", label: "Timing missing", format: "integer" },
      { key: "output_intervals_skipped", label: "Output intervals skipped", format: "integer" },
    ],
  },
];

export const NdiInputNode = defineNode({
  type: type_e.ndi_input,
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
  type: type_e.ndi_output,
  title: "NDI Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(ndiOutputStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    source_name: () => new FocusTrackingStringInterface("Sender Name", ""),
  },
  outputs: {},
});
