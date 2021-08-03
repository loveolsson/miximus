import { Node } from "@baklavajs/core";

export class FrameBufferNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "framebuffer";
    this.name = "";
    //this.addInputInterface("size", "TextOption", 0, { type: "vec2i" });
    this.addOutputInterface("fb", { type: "framebuffer" });
  }
}
