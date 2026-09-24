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
    status: () => new NodeStatusInterface(browserStatus),
    reload: () => new NodeActionInterface("Reload", "reload"),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    url: () => new FocusTrackingStringInterface("URL", "about:blank"),
    size: () =>
      new Vec2Interface("Size", [1920, 1080], { precision: 0, step: 1, min: 1, max: 4096 })
        .use(setType, t_vec2)
        .setPort(false),
  },
  outputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
