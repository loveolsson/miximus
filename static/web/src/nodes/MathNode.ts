import { Node } from "@baklavajs/core";

export class MathNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "math_add_i64";
    this.name = "Math";
    this.addInputInterface("a", "Number", 1);
    this.addInputInterface("b", "Number", 10);
    // this.addOption("Operation", "SelectOption", "Add", undefined, {
    //   items: ["Add", "Subtract"],
    // });
    this.addOutputInterface("res");
  }
}
