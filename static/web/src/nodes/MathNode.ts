import { Node } from "@baklavajs/core";

export class MathNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "math_i64";
    this.name = "";
    this.addInputInterface("a", "Number", 1);
    this.addInputInterface("b", "Number", 10);
    this.addOption("operation", "SelectOption", "add", undefined, {
      items: ["add", "sub", "mul", "min", "max"],
    });
    this.addOutputInterface("res");
  }
}
