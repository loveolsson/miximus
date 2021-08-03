import { Node } from "@baklavajs/core";

export class TeleprompterNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "teleprompter";
    this.name = "";
    this.addInputInterface("fb_in", undefined, undefined, {
      type: "framebuffer",
    });
    this.addOutputInterface("fb_out", { type: "framebuffer" });
    this.addOption("text", "InputOption", "");
  }
}
