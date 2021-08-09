import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";

export class DeckLinkInputNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.decklink_input;
    this.name = "";
    this.addOutputInterface("tex", { type: "texture" });
    this.addOption("enabled", "CheckboxOption", false, undefined);
    this.addOption("device_name", "InputOption", "", undefined);
  }
}
