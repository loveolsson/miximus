import { defineNode, NodeInterface } from "@baklavajs/core";
import { NumberInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_texture, t_framebuffer, t_f64, t_vec2, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { Vec2Interface } from "./interfaces";

export const Vec2Node = defineNode({
  type: type_e.vec2,
  title: "Vec2",
  inputs: {
    x: () => new NumberInterface("X", 0).use(setType, t_f64),
    y: () => new NumberInterface("Y", 0).use(setType, t_f64),
  },
  outputs: {
    res: () => new NodeInterface<[number, number]>("Result", [0, 0]).use(setType, t_vec2),
  },
});

export const RectNode = defineNode({
  type: type_e.rect,
  title: "Rect",
  inputs: {
    pos: () => new Vec2Interface("Pos").use(setType, t_vec2),
    size: () => new Vec2Interface("Size").use(setType, t_vec2),
  },
  outputs: {
    res: () => new NodeInterface<null>("Result", null).use(setType, t_rect),
  },
});

export const FrameBufferNode = defineNode({
  type: type_e.framebuffer,
  title: "Framebuffer",
  inputs: {
    size: () => new Vec2Interface("Size", [0, 0]).use(setType, t_vec2),
  },
  outputs: {
    fb: () => new NodeInterface<null>("FB", null).use(setType, t_framebuffer),
  },
});

export const FramebufferToTextureNode = defineNode({
  type: type_e.framebuffer_to_texture,
  title: "FB → Texture",
  inputs: {
    fb: () => new NodeInterface<null>("FB", null).use(setType, t_framebuffer),
  },
  outputs: {
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
