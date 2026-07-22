import type { Editor } from "@baklavajs/core";
import { BaklavaInterfaceTypes } from "@baklavajs/interface-types";
import type { IBaklavaViewModel } from "@baklavajs/renderer-vue";
import { t_texture, t_framebuffer, t_f64, t_vec2, t_rect } from "./interface_types";

import {
  F64MathNode,
  Vec2MathNode,
  F64LerpNode,
  Vec2LerpNode,
  RectLerpNode,
  F64EasingNode,
  F64ClampNode,
  Vec2ClampNode,
  RectClampNode,
} from "./math";
import { Vec2Node, RectNode, FrameBufferNode, FramebufferToTextureNode } from "./utils";
import { DrawBoxNode, InfiniteMultiviewerNode, MixTex2Node } from "./composite";
import {
  SwitchF64_4Node,
  SwitchF64_8Node,
  SwitchVec2_4Node,
  SwitchVec2_8Node,
  SwitchRect_4Node,
  SwitchRect_8Node,
  SwitchTex_4Node,
  SwitchTex_8Node,
} from "./switch";
import { CircleSourceNode, SinusSourceNode } from "./debug";
import { TestPatternNode } from "./generators";
import { ScreenOutputNode } from "./screen";
import { DeckLinkInputNode, DeckLinkOutputNode } from "./decklink";
import { NdiInputNode, NdiOutputNode } from "./ndi";
import { TeleprompterNode } from "./teleprompter";
import TextNode from "./text";

export function register_node_types(editor: Editor): void {
  editor.registerNodeType(F64MathNode, { category: "Math" });
  editor.registerNodeType(Vec2MathNode, { category: "Math" });
  editor.registerNodeType(F64LerpNode, { category: "Math" });
  editor.registerNodeType(Vec2LerpNode, { category: "Math" });
  editor.registerNodeType(RectLerpNode, { category: "Math" });
  editor.registerNodeType(F64EasingNode, { category: "Math" });
  editor.registerNodeType(F64ClampNode, { category: "Math" });
  editor.registerNodeType(Vec2ClampNode, { category: "Math" });
  editor.registerNodeType(RectClampNode, { category: "Math" });
  editor.registerNodeType(Vec2Node, { category: "Utils" });
  editor.registerNodeType(RectNode, { category: "Utils" });
  editor.registerNodeType(FrameBufferNode, { category: "Utils" });
  editor.registerNodeType(FramebufferToTextureNode, { category: "Utils" });
  editor.registerNodeType(DrawBoxNode, { category: "Composite" });
  editor.registerNodeType(InfiniteMultiviewerNode, { category: "Composite" });
  editor.registerNodeType(MixTex2Node, { category: "Composite" });
  editor.registerNodeType(SwitchF64_4Node, { category: "Switch" });
  editor.registerNodeType(SwitchF64_8Node, { category: "Switch" });
  editor.registerNodeType(SwitchVec2_4Node, { category: "Switch" });
  editor.registerNodeType(SwitchVec2_8Node, { category: "Switch" });
  editor.registerNodeType(SwitchRect_4Node, { category: "Switch" });
  editor.registerNodeType(SwitchRect_8Node, { category: "Switch" });
  editor.registerNodeType(SwitchTex_4Node, { category: "Switch" });
  editor.registerNodeType(SwitchTex_8Node, { category: "Switch" });
  editor.registerNodeType(SinusSourceNode, { category: "Generators" });
  editor.registerNodeType(CircleSourceNode, { category: "Generators" });
  editor.registerNodeType(TestPatternNode, { category: "Generators" });
  editor.registerNodeType(ScreenOutputNode, { category: "Output" });
  editor.registerNodeType(DeckLinkInputNode, { category: "Input" });
  editor.registerNodeType(DeckLinkOutputNode, { category: "Output" });
  editor.registerNodeType(NdiInputNode, { category: "Input" });
  editor.registerNodeType(NdiOutputNode, { category: "Output" });
  editor.registerNodeType(TeleprompterNode, { category: "Content" });
  editor.registerNodeType(TextNode, { category: "Content" });
}

export function register_interface_types(baklava: IBaklavaViewModel): void {
  const intfTypes = new BaklavaInterfaceTypes(baklava.editor, {
    viewPlugin: baklava,
  });
  intfTypes.addTypes(t_texture, t_framebuffer, t_f64, t_vec2, t_rect);
}
