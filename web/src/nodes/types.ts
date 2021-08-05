import { Editor } from "@baklavajs/core";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { type_e } from "@/messages";
import { F64MathNode, I64MathNode, Vec2iMathNode, Vec2MathNode } from "./math";
import { ScreenOutputNode } from "./screen";
import { DeckLinkInputNode } from "./decklink";
import { FrameBufferNode } from "./utils";
import { TeleprompterNode } from "./teleprompter";
import { SinusSourceNode } from "./debug";

/**
 * Color-blind optimized palette
 * https://personal.sron.nl/~pault/#fig:scheme_muted
 */
const Color = {
  Indigo: "#332288", // used
  Cyan: "#88ccee", // used
  Teal: "#44aa99", // used
  Green: "#117733", // used
  Olive: "#999933", // used
  Sand: "#ddcc77", // used
  Rose: "#cc6677",
  Wine: "#882255",
  Purple: "#aa4499",
  PaleGray: "#dddddd",
};

export const connectionColorMap = new Map<string, string>([
  ["texture", Color.Indigo],
  ["framebuffer", Color.Cyan],
  ["i64", Color.Teal],
  ["f64", Color.Green],
  ["vec2", Color.Olive],
  ["vec2i", Color.Sand],
]);

export function register_connection_types(iface: InterfaceTypePlugin): void {
  connectionColorMap.forEach((color, name) => {
    iface.addType(name, color);
  });

  iface.addConversion("i64", "f64");
  iface.addConversion("f64", "i64");
  iface.addConversion("i64", "vec2");
  iface.addConversion("f64", "vec2");
  iface.addConversion("vec2i", "vec2");
  iface.addConversion("i64", "vec2i");
  iface.addConversion("f64", "vec2i");
  iface.addConversion("vec2", "vec2i");
  iface.addConversion("framebuffer", "texture");
}

export function register_types(editor: Editor): void {
  editor.registerNodeType(type_e.math_i64, I64MathNode, "Math");
  editor.registerNodeType(type_e.math_f64, F64MathNode, "Math");
  editor.registerNodeType(type_e.math_vec2, Vec2MathNode, "Math");
  editor.registerNodeType(type_e.math_vec2i, Vec2iMathNode, "Math");

  editor.registerNodeType(type_e.screen_output, ScreenOutputNode, "Outputs");
  editor.registerNodeType(type_e.decklink_input, DeckLinkInputNode, "Inputs");

  editor.registerNodeType(type_e.framebuffer, FrameBufferNode, "Util");

  editor.registerNodeType(type_e.teleprompter, TeleprompterNode, "Render");

  editor.registerNodeType(type_e.sinus_source, SinusSourceNode, "Debug");
}
