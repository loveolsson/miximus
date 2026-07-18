import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
import { StatusDropdownInterface, NodeStatusInterface, type NodeStatusSection } from "./interfaces";

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
    title: "Frame statistics",
    fields: [
      { key: "frames_received", label: "Received", format: "integer" },
      { key: "frames_missing", label: "Missing", format: "integer" },
      { key: "no_input_source_frames", label: "No source", format: "integer" },
      { key: "upload_slot_drops", label: "Upload slot drops", format: "integer" },
      { key: "available_video_frames", label: "Available", format: "integer" },
    ],
  },
];

const decklinkOutputStatus: readonly NodeStatusSection[] = [
  {
    title: "Output",
    fields: [
      { key: "connected", label: "Connection" },
      { key: "active_format", label: "Active format" },
      { key: "playback_busy", label: "Playback", format: "busy" },
      { key: "output_pixel_format", label: "Pixel format" },
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
    title: "Frame statistics",
    fields: [
      { key: "frames_completed", label: "Completed", format: "integer" },
      { key: "frames_displayed_late", label: "Displayed late", format: "integer" },
      { key: "frames_dropped", label: "Dropped", format: "integer" },
      { key: "frames_flushed", label: "Flushed", format: "integer" },
      { key: "buffered_video_frames", label: "Buffered", format: "integer" },
      { key: "render_target_drops", label: "Render target drops", format: "integer" },
    ],
  },
];

export const DeckLinkInputNode = defineNode({
  type: type_e.decklink_input,
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
  type: type_e.decklink_output,
  title: "DeckLink Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(decklinkOutputStatus),
    enabled: () => new CheckboxInterface("Enabled", false).setPort(false),
    device_name: () => new StatusDropdownInterface("Device", "device_names"),
    display_mode: () => new StatusDropdownInterface("Display Mode", "display_modes"),
  },
  outputs: {},
});
