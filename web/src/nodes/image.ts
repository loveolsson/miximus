import { defineNode, NodeInterface } from "@baklavajs/core";
import { TextInputInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { node_type_e } from "./node_type";
import { t_texture } from "./interface_types";

export const ImageNode = defineNode({
  type: node_type_e.image,
  title: "Image",
  inputs: {
    file_path: () => new TextInputInterface("File Path", "").setPort(false),
  },
  outputs: {
    texture: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
