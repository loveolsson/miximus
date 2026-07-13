import { markRaw } from "vue";
import { NumberInterface } from "@baklavajs/renderer-vue";
import NumericComponent from "./options/NumericOption.vue";

export interface NumericOptions {
  precision?: number;
  step?: number;
  min?: number;
  max?: number;
}

/** Number input with domain-specific display precision and button step. */
export class NumericInterface extends NumberInterface {
  readonly precision: number;
  readonly step: number;

  constructor(name: string, defaultValue = 0, options: NumericOptions = {}) {
    super(name, defaultValue, options.min, options.max);
    this.precision = options.precision ?? 2;
    this.step = options.step ?? 0.1;
    this.setComponent(markRaw(NumericComponent));
  }

  setNumericValue(value: number) {
    this.value = value;
  }
}
