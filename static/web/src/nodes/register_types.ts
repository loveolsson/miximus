import { Editor } from "@baklavajs/core";
import { MathNode } from "./MathNode";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { type_e } from "@/messages";

export function register_connection_types(iface: InterfaceTypePlugin) {
  iface.addType("Rectangle", "#");
}

export function register_types(editor: Editor) {
  editor.registerNodeType(type_e.math_i64, MathNode, "math");
}
