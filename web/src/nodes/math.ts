import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { t_f64, t_vec2, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { DropdownInterface, NumericInterface } from "./interfaces";

const scalarOptions = { precision: 2, step: 0.1 } as const;
const factorOptions = { precision: 2, step: 0.05, min: 0, max: 1 } as const;

const easingOptions = [
  { id: "linear", label: "Linear" },
  { id: "quadratic_in", label: "Quadratic In" },
  { id: "quadratic_out", label: "Quadratic Out" },
  { id: "quadratic_in_out", label: "Quadratic In/Out" },
  { id: "cubic_in", label: "Cubic In" },
  { id: "cubic_out", label: "Cubic Out" },
  { id: "cubic_in_out", label: "Cubic In/Out" },
  { id: "quartic_in", label: "Quartic In" },
  { id: "quartic_out", label: "Quartic Out" },
  { id: "quartic_in_out", label: "Quartic In/Out" },
  { id: "quintic_in", label: "Quintic In" },
  { id: "quintic_out", label: "Quintic Out" },
  { id: "quintic_in_out", label: "Quintic In/Out" },
  { id: "sine_in", label: "Sine In" },
  { id: "sine_out", label: "Sine Out" },
  { id: "sine_in_out", label: "Sine In/Out" },
  { id: "circular_in", label: "Circular In" },
  { id: "circular_out", label: "Circular Out" },
  { id: "circular_in_out", label: "Circular In/Out" },
  { id: "exponential_in", label: "Exponential In" },
  { id: "exponential_out", label: "Exponential Out" },
  { id: "exponential_in_out", label: "Exponential In/Out" },
  { id: "elastic_in", label: "Elastic In" },
  { id: "elastic_out", label: "Elastic Out" },
  { id: "elastic_in_out", label: "Elastic In/Out" },
  { id: "back_in", label: "Back In" },
  { id: "back_out", label: "Back Out" },
  { id: "back_in_out", label: "Back In/Out" },
  { id: "bounce_in", label: "Bounce In" },
  { id: "bounce_out", label: "Bounce Out" },
  { id: "bounce_in_out", label: "Bounce In/Out" },
] as const;

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
    allow_overshoot: () => new CheckboxInterface("Allow overshoot", false).setPort(false),
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
    allow_overshoot: () => new CheckboxInterface("Allow overshoot", false).setPort(false),
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
    allow_overshoot: () => new CheckboxInterface("Allow overshoot", false).setPort(false),
  },
  outputs: {
    res: () => new NodeInterface<null>("Result", null).use(setType, t_rect),
  },
});

export const F64EasingNode = defineNode({
  type: type_e.easing_f64,
  title: "Easing",
  inputs: {
    t: () => new NumericInterface("T", 0, factorOptions).use(setType, t_f64),
    easing: () => new DropdownInterface("Easing", "linear", easingOptions),
  },
  outputs: {
    res: () => new NodeInterface<number>("Result", 0).use(setType, t_f64),
  },
});
