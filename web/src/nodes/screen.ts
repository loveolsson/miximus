import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
import {
  StatusDropdownInterface,
  NodeStatusInterface,
  Vec2Interface,
  type NodeStatusSection,
} from "./interfaces";

const screenStatus: readonly NodeStatusSection[] = [
  {
    title: "Screen",
    fields: [{ key: "connected", label: "Connection" }],
  },
];

const pixelPositionOptions = { precision: 0, step: 1 } as const;
const pixelSizeOptions = { precision: 0, step: 1, min: 100 } as const;

export const ScreenOutputNode = defineNode({
  type: type_e.screen_output,
  title: "Screen Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(screenStatus),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    fullscreen: () => new CheckboxInterface("Fullscreen", false).setPort(false),
    monitor_id: () => new StatusDropdownInterface("Monitor", "monitors"),
    position: () => new Vec2Interface("Position", [0, 0], pixelPositionOptions).setPort(false),
    size: () => new Vec2Interface("Size", [100, 100], pixelSizeOptions).setPort(false),
  },
  outputs: {},
});
