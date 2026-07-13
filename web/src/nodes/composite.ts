import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_texture, t_framebuffer, t_f64, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { NumericInterface } from "./interfaces";

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
