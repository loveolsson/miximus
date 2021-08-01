import { Node } from "@baklavajs/core";

export class DeckLinkInputNode extends Node {
  type: string;
  name: string;

  constructor() {
    super();
    this.type = "decklink_input";
    this.name = "";
    this.addOutputInterface("tex", { type: "texture" });
    this.addOption("enabled", "CheckboxOption", false, undefined);

    this.addOption("device_name", "InputOption", "", undefined);
  }
}
