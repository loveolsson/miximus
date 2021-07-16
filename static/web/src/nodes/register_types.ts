import { Editor } from "@baklavajs/core";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { type_e } from "@/messages";
import { F64MathNode, I64MathNode, Vec2MathNode } from "./MathNode";

/**
 * https://personal.sron.nl/~pault/#fig:scheme_muted
 * Indigo    #332288
 * Cyan      #88ccee
 * Teal      #44aa99 *
 * Green     #117733 *
 * Olive     #999933 *
 * Sand      #ddcc77
 * Rose      #cc6677
 * Wine      #882255
 * Purple    #aa4499
 * Pale Gray #dddddd
 */

export function register_connection_types(iface: InterfaceTypePlugin) {
  iface.addType("i64", "#44aa99");
  iface.addType("f64", "#117733");
  iface.addType("vec2", "#999933");

  iface.addConversion("i64", "f64");
  iface.addConversion("f64", "i64");
  iface.addConversion("i64", "vec2");
  iface.addConversion("f64", "vec2");
}

export function register_types(editor: Editor) {
  editor.registerNodeType(type_e.math_i64, I64MathNode, "Math");
  editor.registerNodeType(type_e.math_f64, F64MathNode, "Math");
  editor.registerNodeType(type_e.math_vec2, Vec2MathNode, "Math");
}
