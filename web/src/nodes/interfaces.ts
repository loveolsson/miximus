import { markRaw, reactive } from "vue";
import { NodeInterface } from "@baklavajs/core";
import type { NodeData } from "./status_store";

import FocusTrackingStringComponent from "./options/FocusTrackingStringOption.vue";
import FocusTrackingNumberComponent from "./options/FocusTrackingNumberOption.vue";
import Vec2Component from "./options/Vec2Option.vue";
import StatusDropdownComponent from "./options/StatusDropdownOption.vue";
import NodeStatusComponent from "./options/NodeStatusIndicator.vue";

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
 * Number input with the same focus-tracking behaviour as above.
 */
export class FocusTrackingNumberInterface extends NodeInterface<number> {
  constructor(name: string, defaultValue = 0) {
    super(name, defaultValue);
    this.setComponent(markRaw(FocusTrackingNumberComponent));
    this.setPort(false);
  }
}

/**
 * Two-component vector input.  Has a port by default so it can also
 * receive a connection; the component shows when unconnected.
 */
export class Vec2Interface extends NodeInterface<[number, number]> {
  constructor(name: string, defaultValue: [number, number] = [0, 0]) {
    super(name, defaultValue);
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

/** Returns true if the interface carries a nodeData property. */
export function has_node_data(
  intf: NodeInterface,
): intf is StatusDropdownInterface | NodeStatusInterface {
  return "nodeData" in intf;
}
