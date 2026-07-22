import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_texture, t_framebuffer, t_f64, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { DropdownInterface, NumericInterface } from "./interfaces";

export const DrawBoxNode = defineNode({
  type: type_e.draw_box,
  title: "Draw Box",
  inputs: {
    fb_in: () => new NodeInterface<null>("FB In", null).use(setType, t_framebuffer),
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
    rect: () => new NodeInterface<null>("Rect", null).use(setType, t_rect),
    opacity: () =>
      new NumericInterface("Opacity", 1, { precision: 2, step: 0.05, min: 0, max: 1 }).use(
        setType,
        t_f64,
      ),
  },
  outputs: {
    fb_out: () => new NodeInterface<null>("FB Out", null).use(setType, t_framebuffer),
  },
});

export const InfiniteMultiviewerNode = defineNode({
  type: type_e.infinite_multiviewer,
  title: "Infinite Multiviewer",
  inputs: {
    fb_in: () => new NodeInterface<null>("FB In", null).use(setType, t_framebuffer),
    tex: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
  outputs: {
    fb_out: () => new NodeInterface<null>("FB Out", null).use(setType, t_framebuffer),
  },
});

export const MixTex2Node = defineNode({
  type: type_e.mix_tex_2,
  title: "Mix A/B",
  inputs: {
    fb_in: () => new NodeInterface<null>("FB", null).use(setType, t_framebuffer),
    a: () => new NodeInterface<null>("A", null).use(setType, t_texture),
    b: () => new NodeInterface<null>("B", null).use(setType, t_texture),
    t: () =>
      new NumericInterface("T", 0, { precision: 2, step: 0.05, min: 0, max: 1 }).use(
        setType,
        t_f64,
      ),
    blend_mode: () =>
      new DropdownInterface("Blend mode", "video", [
        { id: "video", label: "Video" },
        { id: "linear", label: "Linear light" },
      ]),
  },
  outputs: {
    fb_out: () => new NodeInterface<null>("FB", null).use(setType, t_framebuffer),
  },
});
