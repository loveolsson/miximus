import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_f64, t_vec2, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { DropdownInterface, NumericInterface } from "./interfaces";

const scalarOptions = { precision: 2, step: 0.1 } as const;
const factorOptions = { precision: 2, step: 0.05, min: 0, max: 1 } as const;

export const F64MathNode = defineNode({
  type: type_e.math_f64,
  title: "F64 Math",
  inputs: {
    a: () => new NumericInterface("A", 0, scalarOptions).use(setType, t_f64),
    b: () => new NumericInterface("B", 0, scalarOptions).use(setType, t_f64),
    operation: () => new DropdownInterface("Operation", "add", ["add", "sub", "mul", "min", "max"]),
  },
  outputs: {
    res: () => new NodeInterface<number>("Result", 0).use(setType, t_f64),
  },
});

export const Vec2MathNode = defineNode({
  type: type_e.math_vec2,
  title: "Vec2 Math",
  inputs: {
    a: () => new NodeInterface<[number, number]>("A", [0, 0]).use(setType, t_vec2),
    b: () => new NodeInterface<[number, number]>("B", [0, 0]).use(setType, t_vec2),
    operation: () => new DropdownInterface("Operation", "add", ["add", "sub", "mul", "min", "max"]),
  },
  outputs: {
    res: () => new NodeInterface<[number, number]>("Result", [0, 0]).use(setType, t_vec2),
  },
});

export const F64LerpNode = defineNode({
  type: type_e.lerp_f64,
  title: "F64 Lerp",
  inputs: {
    a: () => new NumericInterface("A", 0, scalarOptions).use(setType, t_f64),
    b: () => new NumericInterface("B", 0, scalarOptions).use(setType, t_f64),
    t: () => new NumericInterface("T", 0, factorOptions).use(setType, t_f64),
  },
  outputs: {
    res: () => new NodeInterface<number>("Result", 0).use(setType, t_f64),
  },
});

export const Vec2LerpNode = defineNode({
  type: type_e.lerp_vec2,
  title: "Vec2 Lerp",
  inputs: {
    a: () => new NodeInterface<[number, number]>("A", [0, 0]).use(setType, t_vec2),
    b: () => new NodeInterface<[number, number]>("B", [0, 0]).use(setType, t_vec2),
    t: () => new NumericInterface("T", 0, factorOptions).use(setType, t_f64),
  },
  outputs: {
    res: () => new NodeInterface<[number, number]>("Result", [0, 0]).use(setType, t_vec2),
  },
});

export const RectLerpNode = defineNode({
  type: type_e.lerp_rect,
  title: "Rect Lerp",
  inputs: {
    a: () => new NodeInterface<null>("A", null).use(setType, t_rect),
    b: () => new NodeInterface<null>("B", null).use(setType, t_rect),
    t: () => new NumericInterface("T", 0, factorOptions).use(setType, t_f64),
  },
  outputs: {
    res: () => new NodeInterface<null>("Result", null).use(setType, t_rect),
  },
});
