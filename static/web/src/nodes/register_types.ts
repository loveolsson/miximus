import { Editor, Node } from "@baklavajs/core";
import { MathNode } from "./MathNode";
import { DisplayNode } from "./DisplayNode";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";

export function register_connection_types(iface: InterfaceTypePlugin) {
  iface.addType("Rectangle", "#");
}

export function register_types(editor: Editor) {
  editor.registerNodeType("MathNode", MathNode, "math");
  editor.registerNodeType("DisplayNode", DisplayNode, "something");
}
