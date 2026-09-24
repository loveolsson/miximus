import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { node_type_e } from "./node_type";
import { t_texture, t_vec2 } from "./interface_types";
import {
  FocusTrackingStringInterface,
  Vec2Interface,
  NodeStatusInterface,
  NodeActionInterface,
  type NodeStatusSection,
} from "./interfaces";

const browserStatus: readonly NodeStatusSection[] = [
  {
    title: "Browser",
    fields: [
      { key: "cef_state", label: "State" },
      { key: "cef_error", label: "Error" },
      { key: "cef_paints", label: "Received", format: "integer" },
      { key: "cef_copies", label: "Copied", format: "integer" },
      { key: "cef_capacity_drops", label: "Capacity drops", format: "integer" },
      { key: "cef_restarts", label: "Restarts", format: "integer" },
      { key: "cef_timing_rejections", label: "Program-time rejections", format: "integer" },
      { key: "cef_timing_error", label: "Program-time error" },
    ],
  },
  {
    title: "Texture inputs",
    fields: [
      { key: "cef_inputs_state", label: "State" },
      { key: "cef_inputs_error", label: "Error" },
      { key: "cef_inputs_active", label: "Subscribed", format: "integer" },
      { key: "cef_inputs_submitted", label: "Submitted", format: "integer" },
      { key: "cef_inputs_delivered", label: "Delivered", format: "integer" },
      { key: "cef_inputs_drops", label: "Dropped", format: "integer" },
      { key: "cef_inputs_held", label: "Held buffers", format: "integer" },
      { key: "cef_inputs_reserved_bytes", label: "Reserved bytes", format: "integer" },
    ],
  },
  {
    title: "Timed queue",
    fields: [
      { key: "source_queue_depth", label: "Depth", format: "integer" },
      { key: "source_queue_overflow_drops", label: "Overflow drops", format: "integer" },
      { key: "source_queue_selection_drops", label: "Selection drops", format: "integer" },
      { key: "source_queue_repeated", label: "Repeats", format: "integer" },
      { key: "source_queue_missing", label: "Missing", format: "integer" },
    ],
  },
];

export const CefBrowserNode = defineNode({
  type: node_type_e.cef_browser,
  title: "Browser",
  inputs: {
    input_0: () => new NodeInterface<null>("Input 0", null).use(setType, t_texture),
    input_1: () => new NodeInterface<null>("Input 1", null).use(setType, t_texture),
    input_2: () => new NodeInterface<null>("Input 2", null).use(setType, t_texture),
    input_3: () => new NodeInterface<null>("Input 3", null).use(setType, t_texture),
    input_4: () => new NodeInterface<null>("Input 4", null).use(setType, t_texture),
    input_5: () => new NodeInterface<null>("Input 5", null).use(setType, t_texture),
    input_6: () => new NodeInterface<null>("Input 6", null).use(setType, t_texture),
    input_7: () => new NodeInterface<null>("Input 7", null).use(setType, t_texture),

    status: () => new NodeStatusInterface(browserStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    url: () => new FocusTrackingStringInterface("URL", "about:blank"),
    size: () =>
      new Vec2Interface("Size", [1920, 1080], { precision: 0, step: 1, min: 1, max: 4096 })
        .use(setType, t_vec2)
        .setPort(false),
    reload: () =>
      new NodeActionInterface("Reload", [
        { label: "Reload ↻", action: "reload" },
        { label: "Force ↻", action: "reload", payload: { ignore_cache: true } },
      ]),
  },
  outputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
