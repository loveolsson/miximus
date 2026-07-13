import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_texture, t_framebuffer, t_f64, t_vec2, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { NumericInterface, Vec2Interface } from "./interfaces";

const scalarOptions = { precision: 2, step: 0.1 } as const;
const normalizedOptions = { precision: 2, step: 0.05 } as const;
const framebufferSizeOptions = { precision: 0, step: 1, min: 256, max: 4096 } as const;

export const Vec2Node = defineNode({
  type: type_e.vec2,
  title: "Vec2",
  inputs: {
    x: () => new NumericInterface("X", 0, scalarOptions).use(setType, t_f64),
    y: () => new NumericInterface("Y", 0, scalarOptions).use(setType, t_f64),
  },
  outputs: {
    res: () => new NodeInterface<[number, number]>("Result", [0, 0]).use(setType, t_vec2),
  },
});

export const RectNode = defineNode({
  type: type_e.rect,
  title: "Rect",
  inputs: {
    pos: () => new Vec2Interface("Pos", [0, 0], normalizedOptions).use(setType, t_vec2),
    size: () => new Vec2Interface("Size", [1, 1], normalizedOptions).use(setType, t_vec2),
  },
  outputs: {
    res: () => new NodeInterface<null>("Result", null).use(setType, t_rect),
  },
});

export const FrameBufferNode = defineNode({
  type: type_e.framebuffer,
  title: "Framebuffer",
  inputs: {
    size: () =>
      new Vec2Interface("Size", [1920, 1080], framebufferSizeOptions).use(setType, t_vec2),
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
