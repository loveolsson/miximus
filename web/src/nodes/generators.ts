import { defineNode, NodeInterface } from "@baklavajs/core";
import { setType } from "@baklavajs/interface-types";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { type_e } from "@/messages";
import { t_texture, t_vec2 } from "./interface_types";
import { DropdownInterface, Vec2Interface } from "./interfaces";

const resolutionOptions = { precision: 0, step: 1, min: 1, max: 16384 } as const;

const patterns = [
  { id: "smpte_color_bars", label: "SMPTE 75% Color Bars" },
  { id: "ebu_color_bars", label: "EBU 75% Color Bars" },
  { id: "black_field", label: "Black Field" },
  { id: "white_field", label: "White Field" },
  { id: "red_field", label: "Red Field" },
  { id: "green_field", label: "Green Field" },
  { id: "blue_field", label: "Blue Field" },
  { id: "grayscale_ramp", label: "Grayscale Ramp" },
  { id: "crosshatch", label: "Crosshatch" },
  { id: "checkerboard", label: "Checkerboard" },
  { id: "multiburst", label: "Multiburst" },
  { id: "zone_plate", label: "Zone Plate" },
] as const;

export const TestPatternNode = defineNode({
  type: type_e.test_pattern,
  title: "Test pattern",
  inputs: {
    resolution: () =>
      new Vec2Interface("Resolution", [1920, 1080], resolutionOptions)
        .use(setType, t_vec2)
        .setPort(false),
    pattern: () => new DropdownInterface("Pattern", "smpte_color_bars", patterns),
    show_logo: () => new CheckboxInterface("Show logo", false).setPort(false),
  },
  outputs: {
    texture: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
