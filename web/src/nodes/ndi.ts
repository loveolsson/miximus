import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface, TextInputInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture } from "./interface_types";
import { type_e } from "@/messages";
import { StatusDropdownInterface, NodeStatusInterface } from "./interfaces";

export const NdiInputNode = defineNode({
  type: type_e.ndi_input,
  title: "NDI Input",
  inputs: {
    status: () => new NodeStatusInterface(),
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
    status: () => new NodeStatusInterface(),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    source_name: () => new TextInputInterface("Sender Name", "").setPort(false),
  },
  outputs: {},
});
