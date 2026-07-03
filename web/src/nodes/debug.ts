import { defineNode, NodeInterface } from "@baklavajs/core";
import { NumberInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_f64 } from "./interface_types";
import { type_e } from "@/messages";

export const SinusSourceNode = defineNode({
  type: type_e.sinus_source,
  title: "Sinus Source",
  inputs: {
    size: () => new NumberInterface("Size", 0).setPort(false),
    center: () => new NumberInterface("Center", 0).setPort(false),
    speed: () => new NumberInterface("Speed", 0).setPort(false),
  },
  outputs: {
    res: () => new NodeInterface<number>("Result", 0).use(setType, t_f64),
  },
});
