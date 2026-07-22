import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { type_e } from "@/messages";
import { NumericInterface } from "./interfaces";
import { t_f64, t_rect, t_texture, t_vec2 } from "./interface_types";

function createSwitchNode<T>(
  type: type_e,
  title: string,
  slotCount: 4 | 8,
  outputKey: string,
  createInterface: (label: string) => NodeInterface<T>,
) {
  const active = () =>
    new NumericInterface("Active input", 1, {
      precision: 0,
      step: 1,
      min: 1,
      max: slotCount,
    });

  const outputs: Record<string, () => NodeInterface<T>> = {
    [outputKey]: () => createInterface("Output"),
  };

  if (slotCount === 4) {
    return defineNode({
      type,
      title,
      inputs: {
        a: () => createInterface("A"),
        b: () => createInterface("B"),
        c: () => createInterface("C"),
        d: () => createInterface("D"),
        active,
      },
      outputs,
    });
  }

  return defineNode({
    type,
    title,
    inputs: {
      a: () => createInterface("A"),
      b: () => createInterface("B"),
      c: () => createInterface("C"),
      d: () => createInterface("D"),
      e: () => createInterface("E"),
      f: () => createInterface("F"),
      g: () => createInterface("G"),
      h: () => createInterface("H"),
      active,
    },
    outputs,
  });
}

const createNumberInterface = (label: string) =>
  new NodeInterface<number>(label, 0).use(setType, t_f64);
const createVec2Interface = (label: string) =>
  new NodeInterface<[number, number]>(label, [0, 0]).use(setType, t_vec2);
const createRectInterface = (label: string) =>
  new NodeInterface<null>(label, null).use(setType, t_rect);
const createTextureInterface = (label: string) =>
  new NodeInterface<null>(label, null).use(setType, t_texture);

export const SwitchF64_4Node = createSwitchNode(
  type_e.switch_f64_4,
  "Switch Number 4",
  4,
  "res",
  createNumberInterface,
);
export const SwitchF64_8Node = createSwitchNode(
  type_e.switch_f64_8,
  "Switch Number 8",
  8,
  "res",
  createNumberInterface,
);
export const SwitchVec2_4Node = createSwitchNode(
  type_e.switch_vec2_4,
  "Switch Vec2 4",
  4,
  "res",
  createVec2Interface,
);
export const SwitchVec2_8Node = createSwitchNode(
  type_e.switch_vec2_8,
  "Switch Vec2 8",
  8,
  "res",
  createVec2Interface,
);
export const SwitchRect_4Node = createSwitchNode(
  type_e.switch_rect_4,
  "Switch Rect 4",
  4,
  "res",
  createRectInterface,
);
export const SwitchRect_8Node = createSwitchNode(
  type_e.switch_rect_8,
  "Switch Rect 8",
  8,
  "res",
  createRectInterface,
);
export const SwitchTex_4Node = createSwitchNode(
  type_e.switch_tex_4,
  "Switch Texture 4",
  4,
  "tex",
  createTextureInterface,
);
export const SwitchTex_8Node = createSwitchNode(
  type_e.switch_tex_8,
  "Switch Texture 8",
  8,
  "tex",
  createTextureInterface,
);
