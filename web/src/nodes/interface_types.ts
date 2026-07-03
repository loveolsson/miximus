/**
 * Central registry of NodeInterfaceType instances.
 * Import the `t_*` constants here wherever setType is needed.
 * Conversions are registered once at module-load time.
 */
import { NodeInterfaceType } from "@baklavajs/interface-types";

/**
 * Color-blind optimized palette (https://personal.sron.nl/~pault/#fig:scheme_muted)
 * Maps interface type name → CSS stroke color used by ColoredConnectionWrapper.
 */
export const connectionColorMap = new Map<string, string>([
  ["texture", "#332288"],
  ["framebuffer", "#88ccee"],
  ["f64", "#117733"],
  ["vec2", "#999933"],
  ["rect", "#cc6677"],
]);

export const t_texture = new NodeInterfaceType<null>("texture");
export const t_framebuffer = new NodeInterfaceType<null>("framebuffer");
export const t_f64 = new NodeInterfaceType<number>("f64");
export const t_vec2 = new NodeInterfaceType<[number, number]>("vec2");
export const t_rect = new NodeInterfaceType<null>("rect");

// Allowed implicit conversions for connections.
t_f64.addConversion(t_vec2);
t_framebuffer.addConversion(t_texture);
