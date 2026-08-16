import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { node_type_e } from "./node_type";
import { createFillModeInterface } from "./fill_mode";
import {
  StatusDropdownInterface,
  NodeStatusInterface,
  Vec2Interface,
  type NodeStatusSection,
} from "./interfaces";

const screenStatus: readonly NodeStatusSection[] = [
  {
    title: "Screen",
    fields: [
      { key: "connected", label: "Connection" },
      { key: "clock_quality", label: "Clock quality" },
      { key: "measured_refresh_hz", label: "Measured refresh (Hz)", precision: 3 },
      { key: "swaps_completed", label: "Swaps", format: "integer" },
      { key: "output_intervals_skipped", label: "Skipped refresh intervals", format: "integer" },
      { key: "completion_interval_max_us", label: "Longest swap interval (µs)", format: "integer" },
    ],
  },
  {
    title: "Program timing",
    fields: [
      { key: "frames_submitted", label: "Frames submitted", format: "integer" },
      { key: "program_frames_repeated", label: "Frames repeated", format: "integer" },
      { key: "program_frames_missing", label: "Frames missing", format: "integer" },
      { key: "program_timing_drops", label: "Timing drops", format: "integer" },
      { key: "program_queue_overflow_drops", label: "Queue overflow drops", format: "integer" },
      { key: "queued_frames", label: "Queued frames", format: "integer" },
      { key: "output_latency_us", label: "Output latency (µs)", format: "integer" },
      { key: "program_selection_offset_us", label: "Selection offset (µs)", format: "integer" },
    ],
  },
  {
    title: "Render slots",
    fields: [
      { key: "render_slots", label: "Slots", format: "integer" },
      { key: "render_slots_free", label: "Free", format: "integer" },
      { key: "render_slots_retiring", label: "Retiring", format: "integer" },
      { key: "render_acquire_misses", label: "Acquisition misses", format: "integer" },
    ],
  },
];

const pixelPositionOptions = { precision: 0, step: 1 } as const;
const pixelSizeOptions = { precision: 0, step: 1, min: 100 } as const;

export const ScreenOutputNode = defineNode({
  type: node_type_e.screen_output,
  title: "Screen Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(screenStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    fullscreen: () => new CheckboxInterface("Fullscreen", false).setPort(false),
    fill_mode: () => createFillModeInterface("scale"),
    monitor_id: () => new StatusDropdownInterface("Monitor", "monitors"),
    position: () => new Vec2Interface("Position", [0, 0], pixelPositionOptions).setPort(false),
    size: () => new Vec2Interface("Size", [100, 100], pixelSizeOptions).setPort(false),
  },
  outputs: {},
});
