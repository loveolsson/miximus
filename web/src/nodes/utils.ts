import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export class Vec2Node extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.vec2;
    this.name = "";
    this.addInputInterface("x", "NumberOption", 0, { type: "f64" });
    this.addInputInterface("y", "NumberOption", 0, { type: "f64" });
    this.addOutputInterface("res", { type: "vec2" });
  }
}

export class RectNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.rect;
    this.name = "";
    this.addInputInterface("pos", "Vec2Option", undefined, { type: "vec2" });
    this.addInputInterface("size", "Vec2Option", undefined, { type: "vec2" });
    this.addOutputInterface("res", { type: "rect" });
  }
}

export class FrameBufferNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.framebuffer;
    this.name = "";
    this.addInputInterface("size", "Vec2Option", [0, 0], { type: "vec2" });
    this.addOutputInterface("fb", { type: "framebuffer" });
  }
}

export class FramebufferToTextureNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.framebuffer_to_texture;
    this.name = "";
    this.addInputInterface("fb", undefined, undefined, { type: "framebuffer" });
    this.addOutputInterface("tex", { type: "texture" });
  }
}
