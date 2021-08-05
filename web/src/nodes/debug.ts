import { Node } from "@baklavajs/core";

export class SinusSourceNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "sinus_source";
    this.name = "";
    this.addOption("size", "NumberOption", 0, undefined);
    this.addOption("center", "NumberOption", 0, undefined);
    this.addOption("speed", "NumberOption", 0, undefined);
    this.addOutputInterface("res", { type: "f64" });
  }
}
