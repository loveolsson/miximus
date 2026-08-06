import { defineNode, NodeInterface } from "@baklavajs/core";
import { markRaw } from "vue";
import { node_type_e } from "./node_type";
import { NumericInterface, Vec2Interface } from "./interfaces";
import FrameRateOption from "./options/FrameRateOption.vue";
import type { frame_rate_s } from "@/generated/json_contracts";

export type { frame_rate_s } from "@/generated/json_contracts";

export interface FrameRateChoice {
  readonly label: string;
  readonly value: frame_rate_s;
}

const frameRates: readonly FrameRateChoice[] = [
  { label: "23.976 fps", value: { numerator: 24_000, denominator: 1_001 } },
  { label: "24 fps", value: { numerator: 24, denominator: 1 } },
  { label: "25 fps", value: { numerator: 25, denominator: 1 } },
  { label: "29.97 fps", value: { numerator: 30_000, denominator: 1_001 } },
  { label: "30 fps", value: { numerator: 30, denominator: 1 } },
  { label: "50 fps", value: { numerator: 50, denominator: 1 } },
  { label: "59.94 fps", value: { numerator: 60_000, denominator: 1_001 } },
  { label: "60 fps", value: { numerator: 60, denominator: 1 } },
];

export class FrameRateInterface extends NodeInterface<frame_rate_s> {
  readonly choices = frameRates;

  constructor() {
    super("Frame rate", { numerator: 60, denominator: 1 });
    this.setComponent(markRaw(FrameRateOption));
    this.setPort(false);
  }
}

export const ApplicationSettingsNode = defineNode({
  type: node_type_e.application_settings,
  title: "Application Settings",
  inputs: {
    frame_rate: () => new FrameRateInterface(),
    default_framebuffer_size: () =>
      new Vec2Interface("Default size", [1920, 1080], {
        precision: 0,
        step: 1,
        min: 256,
        max: 4096,
      }).setPort(false),
    decklink_output_buffer_frames: () =>
      new NumericInterface("Buffered frames", 4, {
        precision: 0,
        step: 1,
        min: 1,
        max: 8,
      }).setPort(false),
    ndi_output_buffer_frames: () =>
      new NumericInterface("Buffered frames", 4, {
        precision: 0,
        step: 1,
        min: 1,
        max: 8,
      }).setPort(false),
    screen_output_buffer_frames: () =>
      new NumericInterface("Buffered frames", 2, {
        precision: 0,
        step: 1,
        min: 1,
        max: 8,
      }).setPort(false),
  },
  outputs: {},
});
