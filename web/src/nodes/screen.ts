import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface, IntegerInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
import { StatusDropdownInterface, NodeStatusInterface } from "./interfaces";

export const ScreenOutputNode = defineNode({
  type: type_e.screen_output,
  title: "Screen Output",
  inputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    status: () => new NodeStatusInterface(),
    enabled: () => new CheckboxInterface("Enabled", false).setPort(false),
    fullscreen: () => new CheckboxInterface("Fullscreen", false).setPort(false),
    monitor_name: () => new StatusDropdownInterface("Monitor", "monitors"),
    posx: () => new IntegerInterface("X", 0).setPort(false),
    posy: () => new IntegerInterface("Y", 0).setPort(false),
    sizex: () => new IntegerInterface("Width", 100).setPort(false),
    sizey: () => new IntegerInterface("Height", 100).setPort(false),
  },
  outputs: {},
});
