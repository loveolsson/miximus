import type { Editor } from "@baklavajs/core";
import { BaklavaInterfaceTypes } from "@baklavajs/interface-types";
import type { IBaklavaViewModel } from "@baklavajs/renderer-vue";
import { t_texture, t_framebuffer, t_f64, t_vec2, t_rect } from "./interface_types";

import { F64MathNode, Vec2MathNode, F64LerpNode, Vec2LerpNode, RectLerpNode } from "./math";
import { Vec2Node, RectNode, FrameBufferNode, FramebufferToTextureNode } from "./utils";
import { DrawBoxNode, InfiniteMultiviewerNode } from "./composite";
import { SinusSourceNode } from "./debug";
import { ScreenOutputNode } from "./screen";
import { DeckLinkInputNode, DeckLinkOutputNode } from "./decklink";
import { TeleprompterNode } from "./teleprompter";
import TextNode from "./text";

export function register_node_types(editor: Editor): void {
  editor.registerNodeType(F64MathNode, { category: "Math" });
  editor.registerNodeType(Vec2MathNode, { category: "Math" });
  editor.registerNodeType(F64LerpNode, { category: "Math" });
  editor.registerNodeType(Vec2LerpNode, { category: "Math" });
  editor.registerNodeType(RectLerpNode, { category: "Math" });
  editor.registerNodeType(Vec2Node, { category: "Utils" });
  editor.registerNodeType(RectNode, { category: "Utils" });
  editor.registerNodeType(FrameBufferNode, { category: "Utils" });
  editor.registerNodeType(FramebufferToTextureNode, { category: "Utils" });
  editor.registerNodeType(DrawBoxNode, { category: "Composite" });
  editor.registerNodeType(InfiniteMultiviewerNode, { category: "Composite" });
  editor.registerNodeType(SinusSourceNode, { category: "Debug" });
  editor.registerNodeType(ScreenOutputNode, { category: "Output" });
  editor.registerNodeType(DeckLinkInputNode, { category: "Input" });
  editor.registerNodeType(DeckLinkOutputNode, { category: "Output" });
  editor.registerNodeType(TeleprompterNode, { category: "Content" });
  editor.registerNodeType(TextNode, { category: "Content" });
}

export function register_interface_types(baklava: IBaklavaViewModel): void {
  const intfTypes = new BaklavaInterfaceTypes(baklava.editor, {
    viewPlugin: baklava,
  });
  intfTypes.addTypes(t_texture, t_framebuffer, t_f64, t_vec2, t_rect);
}
