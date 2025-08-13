import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export default class TextNode extends Node {
  type: type_e;
  name: string;

  public constructor() {
    super();
    this.type = type_e.text;
    this.name = "";

    this.addInputInterface("fb_in", undefined, undefined, {
      type: "framebuffer",
    });
    this.addInputInterface("position", undefined, 0, {
      type: "vec2",
    });
    this.addOutputInterface("fb_out", { type: "framebuffer" });

    this.addOption("text", "FocusTrackingStringOption", "Hello World");
    this.addOption("font_name", "FocusTrackingStringOption", "Arial");
    this.addOption("font_variant", "FocusTrackingStringOption", "Regular");
    this.addOption("font_size", "FocusTrackingNumberOption", 48);
  }
}
