import { DropdownInterface } from "./interfaces";

export type fill_mode_t = "scale" | "fill" | "contain";

const fillModeOptions = [
  { id: "scale", label: "Scale" },
  { id: "fill", label: "Fill" },
  { id: "contain", label: "Contain" },
] as const;

export function createFillModeInterface(defaultValue: fill_mode_t): DropdownInterface {
  return new DropdownInterface("Fill mode", defaultValue, fillModeOptions);
}
