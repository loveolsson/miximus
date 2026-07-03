import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
import { StatusDropdownInterface, NodeStatusInterface } from "./interfaces";

export const DeckLinkInputNode = defineNode({
  type: type_e.decklink_input,
  title: "DeckLink Input",
  inputs: {
    status: () => new NodeStatusInterface(),
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
    status: () => new NodeStatusInterface(),
    enabled: () => new CheckboxInterface("Enabled", false).setPort(false),
    device_name: () => new StatusDropdownInterface("Device", "device_names"),
    display_mode: () => new StatusDropdownInterface("Display Mode", "display_modes"),
  },
  outputs: {},
});
