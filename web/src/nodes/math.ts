import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export class F64MathNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.math_f64;
    this.name = "";
    this.addInputInterface("a", "NumberOption", 0, { type: "f64" });
    this.addInputInterface("b", "NumberOption", 0, { type: "f64" });
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res", { type: "f64" });
  }
}

export class Vec2MathNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.math_vec2;
    this.name = "";
    this.addInputInterface("a", "Vec2Option", [0, 0], { type: "vec2" });
    this.addInputInterface("b", "Vec2Option", [0, 0], { type: "vec2" });
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res", { type: "vec2" });
  }
}

export class F64LerpNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.lerp_f64;
    this.name = "";
    this.addInputInterface("a", "NumberOption", 0, { type: "f64" });
    this.addInputInterface("b", "NumberOption", 0, { type: "f64" });
    this.addInputInterface("t", "NumberOption", 0, { type: "f64" });

    this.addOutputInterface("res", { type: "f64" });
  }
}

export class Vec2LerpNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.lerp_vec2;
    this.name = "";
    this.addInputInterface("a", "Vec2Option", 0, { type: "vec2" });
    this.addInputInterface("b", "Vec2Option", 0, { type: "vec2" });
    this.addInputInterface("t", "NumberOption", 0, { type: "f64" });

    this.addOutputInterface("res", { type: "vec2" });
  }
}

export class RectLerpNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.lerp_rect;
    this.name = "";
    this.addInputInterface("a", undefined, 0, { type: "rect" });
    this.addInputInterface("b", undefined, 0, { type: "rect" });
    this.addInputInterface("t", "NumberOption", 0, { type: "f64" });

    this.addOutputInterface("res", { type: "rect" });
  }
}
