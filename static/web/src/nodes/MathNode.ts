import { Node } from "@baklavajs/core";

export class I64MathNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "math_i64";
    this.name = "";
    this.addInputInterface("a", "IntegerOption", 0, {type: "i64"});
    this.addInputInterface("b", "IntegerOption", 0, {type: "i64"});
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res", {type: "i64"});

  }
}

export class F64MathNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "math_f64";
    this.name = "";
    this.addInputInterface("a", "NumberOption", 0, {type: "f64"});
    this.addInputInterface("b", "NumberOption", 0, {type: "f64"});
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res", {type: "f64"});
  }
}

export class Vec2MathNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "math_vec2";
    this.name = "";
    this.addInputInterface("a", undefined, 0, {type: "vec2"});
    this.addInputInterface("b", undefined, 0, {type: "vec2"});
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res", {type: "vec2"});
  }
}

