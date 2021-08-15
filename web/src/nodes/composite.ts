import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export class DrawBoxNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.draw_box;
    this.name = "";
    this.addInputInterface("fb_in", undefined, undefined, {
      type: "framebuffer",
    });
    this.addInputInterface("tex", undefined, 0, {
      type: "texture",
    });
    this.addInputInterface("rect", undefined, 0, {
      type: "rect",
    });
    this.addInputInterface("opacity", "NumberOption", 1, {
      type: "f64",
    });
    this.addOutputInterface("fb_out", { type: "framebuffer" });
  }
}

export class InfiniteMultiviewerNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.infinite_multiviewer;
    this.name = "";
    this.addInputInterface("fb_in", undefined, undefined, {
      type: "framebuffer",
    });
    this.addInputInterface("tex", undefined, 0, {
      type: "texture",
    });
    this.addOutputInterface("fb_out", { type: "framebuffer" });
  }
}
