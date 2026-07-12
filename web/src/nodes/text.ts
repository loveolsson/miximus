import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_framebuffer, t_vec2 } from "./interface_types";
import { type_e } from "@/messages";
import {
  FocusTrackingStringInterface,
  FocusTrackingNumberInterface,
  StatusDropdownInterface,
  Vec2Interface,
} from "./interfaces";

const TextNode = defineNode({
  type: type_e.text,
  title: "Text",
  inputs: {
    fb_in: () => new NodeInterface<null>("FB In", null).use(setType, t_framebuffer),
    position: () => new Vec2Interface("Position").use(setType, t_vec2),
    text: () => new FocusTrackingStringInterface("Text", "Hello World"),
    font_name: () => new StatusDropdownInterface("Font", "font_names", "Arial"),
    font_variant: () => new StatusDropdownInterface("Variant", "font_variants", "Regular"),
    font_size: () => new FocusTrackingNumberInterface("Size", 48),
  },
  outputs: {
    fb_out: () => new NodeInterface<null>("FB Out", null).use(setType, t_framebuffer),
  },
});

export default TextNode;
