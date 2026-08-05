import { markRaw, reactive } from "vue";
import { NodeInterface } from "@baklavajs/core";
import type { NodeData } from "./status_store";
import type { NumericOptions } from "./numeric";
import type { node_status_s, settings_option_s, vec2_t } from "@/generated/json_contracts";

export { NumericInterface, type NumericOptions } from "./numeric";

import FocusTrackingStringComponent from "./options/FocusTrackingStringOption.vue";
import Vec2Component from "./options/Vec2Option.vue";
import StatusDropdownComponent from "./options/StatusDropdownOption.vue";
import DropdownComponent from "./options/DropdownOption.vue";
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
export class Vec2Interface extends NodeInterface<vec2_t> {
  readonly numericOptions: NumericOptions;

  constructor(name: string, defaultValue: vec2_t = [0, 0], numericOptions: NumericOptions = {}) {
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
  readonly list_key: StatusOptionKey;

  constructor(name: string, listKey: StatusOptionKey, defaultValue = "") {
    super(name, defaultValue);
    this.list_key = listKey;
    this.nodeData = reactive({ node_id: "" });
    this.setComponent(markRaw(StatusDropdownComponent));
    this.setPort(false);
  }
}

/** Dropdown with a fixed list of values declared by the node definition. */
export class DropdownInterface extends NodeInterface<string> {
  readonly items: readonly settings_option_s[];

  constructor(name: string, defaultValue: string, items: readonly (string | settings_option_s)[]) {
    super(name, defaultValue);
    this.items = items.map((item) => (typeof item === "string" ? { id: item, label: item } : item));
    this.setComponent(markRaw(DropdownComponent));
    this.setPort(false);
  }
}

export type { settings_option_s } from "@/generated/json_contracts";

type StatusOptionKey = {
  [Key in keyof node_status_s]-?: NonNullable<
    node_status_s[Key]
  > extends readonly settings_option_s[]
    ? Key
    : never;
}[keyof node_status_s];

export type NodeStatusFormat = "active" | "busy" | "failure" | "integer" | "locked" | "temperature";

export interface NodeStatusField {
  readonly key: keyof node_status_s;
  readonly label: string;
  readonly format?: NodeStatusFormat;
  readonly precision?: number;
}

export interface NodeStatusSection {
  readonly title: string;
  readonly fields: readonly NodeStatusField[];
}

/** Read-only status indicator with an explicitly declared detail layout. */
export class NodeStatusInterface extends NodeInterface<null> {
  readonly nodeData: NodeData;
  readonly sections: readonly NodeStatusSection[];

  constructor(sections: readonly NodeStatusSection[]) {
    super("Status", null);
    this.sections = sections;
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
