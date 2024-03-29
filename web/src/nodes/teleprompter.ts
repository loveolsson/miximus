import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export class TeleprompterNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.teleprompter;
    this.name = "";
    this.addInputInterface("fb_in", undefined, undefined, {
      type: "framebuffer",
    });
    this.addInputInterface("scroll_pos", "NumberOption", 0, {
      type: "f64",
    });
    this.addInputInterface("rect", undefined, 0, {
      type: "rect",
    });
    this.addOutputInterface("fb_out", { type: "framebuffer" });
    this.addOption("file_path", "InputOption", "");
  }
}
