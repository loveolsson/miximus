import { defineNode, NodeInterface } from "@baklavajs/core";
import { FilePathInterface } from "./interfaces";
import { setType } from "@baklavajs/interface-types";
import { node_type_e } from "./node_type";
import { t_texture } from "./interface_types";

export const ImageNode = defineNode({
  type: node_type_e.image,
  title: "Image",
  inputs: {
    file_path: () => new FilePathInterface("File Path"),
  },
  outputs: {
    texture: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
