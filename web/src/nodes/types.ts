import { Editor } from "@baklavajs/core";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { type_e } from "@/messages";
import {
  F64LerpNode,
  F64MathNode,
  RectLerpNode,
  Vec2LerpNode,
  Vec2MathNode,
} from "./math";
import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import { ScreenOutputNode } from "./screen";
import { DeckLinkInputNode, DeckLinkOutputNode } from "./decklink";
import {
  FrameBufferNode,
  FramebufferToTextureNode,
  RectNode,
  Vec2Node,
} from "./utils";
import { TeleprompterNode } from "./teleprompter";
import { SinusSourceNode } from "./debug";
import Vec2Option from "./options/Vec2Option.vue";
import { DrawBoxNode } from "./composite";

/**
 * Color-blind optimized palette
 * https://personal.sron.nl/~pault/#fig:scheme_muted
 */
const Color = {
  Indigo: "#332288", // used
  Cyan: "#88ccee", // used
  Teal: "#44aa99",
  Green: "#117733", // used
  Olive: "#999933", // used
  Sand: "#ddcc77",
  Rose: "#cc6677", // used
  Wine: "#882255",
  Purple: "#aa4499",
  PaleGray: "#dddddd",
};

export const connectionColorMap = new Map<string, string>([
  ["texture", Color.Indigo],
  ["framebuffer", Color.Cyan],
  ["f64", Color.Green],
  ["vec2", Color.Olive],
  ["rect", Color.Rose],
]);

export function register_connection_types(iface: InterfaceTypePlugin): void {
  connectionColorMap.forEach((color, name) => {
    iface.addType(name, color);
  });

  iface.addConversion("f64", "vec2");
  iface.addConversion("framebuffer", "texture");
}

export function register_option_types(view: ViewPlugin): void {
  view.registerOption("Vec2Option", Vec2Option);
}

export function register_types(editor: Editor): void {
  editor.registerNodeType(type_e.math_f64, F64MathNode, "Math");
  editor.registerNodeType(type_e.math_vec2, Vec2MathNode, "Math");
  editor.registerNodeType(type_e.lerp_f64, F64LerpNode, "Math");
  editor.registerNodeType(type_e.lerp_vec2, Vec2LerpNode, "Math");
  editor.registerNodeType(type_e.lerp_rect, RectLerpNode, "Math");

  editor.registerNodeType(type_e.screen_output, ScreenOutputNode, "Outputs");
  editor.registerNodeType(type_e.decklink_input, DeckLinkInputNode, "Inputs");
  editor.registerNodeType(
    type_e.decklink_output,
    DeckLinkOutputNode,
    "Outputs"
  );

  editor.registerNodeType(type_e.vec2, Vec2Node, "Utils");
  editor.registerNodeType(type_e.rect, RectNode, "Utils");
  editor.registerNodeType(type_e.framebuffer, FrameBufferNode, "Utils");
  editor.registerNodeType(
    type_e.framebuffer_to_texture,
    FramebufferToTextureNode,
    "Utils"
  );

  editor.registerNodeType(type_e.draw_box, DrawBoxNode, "Composite");

  editor.registerNodeType(type_e.teleprompter, TeleprompterNode, "Render");

  editor.registerNodeType(type_e.sinus_source, SinusSourceNode, "Debug");
}
