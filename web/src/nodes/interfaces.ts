import { markRaw, reactive } from "vue";
import { NodeInterface } from "@baklavajs/core";
import type { NodeData } from "./status_store";
import type { NumericOptions } from "./numeric";

export { NumericInterface, type NumericOptions } from "./numeric";

import FocusTrackingStringComponent from "./options/FocusTrackingStringOption.vue";
import Vec2Component from "./options/Vec2Option.vue";
import StatusDropdownComponent from "./options/StatusDropdownOption.vue";
import NodeStatusComponent from "./options/NodeStatusIndicator.vue";
import FontRegistryRefreshComponent from "./options/FontRegistryRefreshOption.vue";

/**
 * Text input that blocks server updates while the user is focused,
 * then applies the latest server value on blur.
 */
export class FocusTrackingStringInterface extends NodeInterface<string> {
  constructor(name: string, defaultValue = "") {
    super(name, defaultValue);
    this.setComponent(markRaw(FocusTrackingStringComponent));
    this.setPort(false);
  }
}

/**
 * Two-component vector input.  Has a port by default so it can also
 * receive a connection; the component shows when unconnected.
 */
export class Vec2Interface extends NodeInterface<[number, number]> {
  readonly numericOptions: NumericOptions;

  constructor(
    name: string,
    defaultValue: [number, number] = [0, 0],
    numericOptions: NumericOptions = {},
  ) {
    super(name, defaultValue);
    this.numericOptions = numericOptions;
    this.setComponent(markRaw(Vec2Component));
  }
}

/**
 * Dropdown whose option list is populated from the live node status
 * for `list_key`. User selection is preserved across disconnects.
 */
export class StatusDropdownInterface extends NodeInterface<string> {
  readonly nodeData: NodeData;
  readonly list_key: string;

  constructor(name: string, listKey: string, defaultValue = "") {
    super(name, defaultValue);
    this.list_key = listKey;
    this.nodeData = reactive({ node_id: "" });
    this.setComponent(markRaw(StatusDropdownComponent));
    this.setPort(false);
  }
}

/**
 * Read-only status indicator (connected dot + active_format).
 * Uses the same nodeData pattern as StatusDropdownInterface.
 */
export class NodeStatusInterface extends NodeInterface<null> {
  readonly nodeData: NodeData;

  constructor() {
    super("Status", null);
    this.nodeData = reactive({ node_id: "" });
    this.setComponent(markRaw(NodeStatusComponent));
    this.setPort(false);
  }
}

/** Application-wide font registry refresh control shown on font-using nodes. */
export class FontRegistryRefreshInterface extends NodeInterface<null> {
  constructor() {
    super("Font Registry", null);
    this.setComponent(markRaw(FontRegistryRefreshComponent));
    this.setPort(false);
  }
}

/** Returns true if the interface carries a nodeData property. */
export function has_node_data(
  intf: NodeInterface,
): intf is StatusDropdownInterface | NodeStatusInterface {
  return "nodeData" in intf;
}
