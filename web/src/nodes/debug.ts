import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { t_f64, t_vec2 } from "./interface_types";
import { type_e } from "@/messages";
import { NumericInterface, Vec2Interface } from "./interfaces";

export const SinusSourceNode = defineNode({
  type: type_e.sinus_source,
  title: "Sinus Source",
  inputs: {
    size: () => new NumericInterface("Size", 1, { precision: 2, step: 0.1 }).setPort(false),
    center: () => new NumericInterface("Center", 0, { precision: 2, step: 0.1 }).setPort(false),
    speed: () => new NumericInterface("Speed", 0.1, { precision: 2, step: 0.05 }).setPort(false),
    phase: () => new NumericInterface("Phase (°)", 0, { precision: 1, step: 1 }).setPort(false),
  },
  outputs: {
    res: () => new NodeInterface<number>("Result", 0).use(setType, t_f64),
  },
});

export const CircleSourceNode = defineNode({
  type: type_e.circle_source,
  title: "Circle Source",
  inputs: {
    size: () => new Vec2Interface("Size", [1, 1], { precision: 2, step: 0.1 }).setPort(false),
    center: () => new Vec2Interface("Center", [0, 0], { precision: 2, step: 0.1 }).setPort(false),
    speed: () => new NumericInterface("Speed", 0.1, { precision: 2, step: 0.05 }).setPort(false),
    phase: () => new NumericInterface("Phase (°)", 0, { precision: 1, step: 1 }).setPort(false),
  },
  outputs: {
    res: () => new NodeInterface<[number, number]>("Result", [0, 0]).use(setType, t_vec2),
  },
});
