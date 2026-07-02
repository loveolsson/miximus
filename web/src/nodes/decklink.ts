import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";
import { make_node_option_data } from "@/nodes/status_store";

export class DeckLinkInputNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.decklink_input;
    this.name = "";
    const nodeData = make_node_option_data();
    this.addOutputInterface("tex", { type: "texture" });
    this.addOption("status_display", "NodeStatusIndicator", null, undefined, {
      nodeData,
    });
    this.addOption("enabled", "CheckboxOption", false, undefined);
    this.addOption("device_name", "StatusDropdownOption", "", undefined, {
      nodeData,
      list_key: "device_names",
    });
  }
}

export class DeckLinkOutputNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.decklink_output;
    this.name = "";
    const nodeData = make_node_option_data();
    this.addInputInterface("tex", undefined, undefined, { type: "texture" });
    this.addOption("status_display", "NodeStatusIndicator", null, undefined, {
      nodeData,
    });
    this.addOption("enabled", "CheckboxOption", false, undefined);
    this.addOption("device_name", "StatusDropdownOption", "", undefined, {
      nodeData,
      list_key: "device_names",
    });
    this.addOption("display_mode", "StatusDropdownOption", "", undefined, {
      nodeData,
      list_key: "display_modes",
    });
  }
}
