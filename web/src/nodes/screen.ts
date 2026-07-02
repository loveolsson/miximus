import { type_e } from "@/messages";
import { Node } from "@baklavajs/core";
import { make_node_option_data } from "@/nodes/status_store";

export class ScreenOutputNode extends Node {
  type: type_e;
  name: string;

  constructor() {
    super();
    this.type = type_e.screen_output;
    this.name = "";
    const nodeData = make_node_option_data();
    this.addInputInterface("tex", undefined, 0, { type: "texture" });
    this.addOption("status_display", "NodeStatusIndicator", null, undefined, {
      nodeData,
    });
    this.addOption("enabled", "CheckboxOption", false, undefined);
    this.addOption("fullscreen", "CheckboxOption", false, undefined);
    this.addOption("monitor_name", "StatusDropdownOption", "", undefined, {
      nodeData,
      list_key: "monitors",
    });

    this.addOption("posx", "IntegerOption", 0, undefined);
    this.addOption("posy", "IntegerOption", 0, undefined);
    this.addOption("sizex", "IntegerOption", 100, undefined, {
      min: 100,
      max: 4096,
    });
    this.addOption("sizey", "IntegerOption", 100, undefined, {
      min: 100,
      max: 4096,
    });
  }
}
