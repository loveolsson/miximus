import { defineNode, NodeInterface } from "@baklavajs/core";
import { NumberInterface, TextInputInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { t_framebuffer, t_f64, t_rect } from "./interface_types";
import { type_e } from "@/messages";
import { FocusTrackingNumberInterface, StatusDropdownInterface } from "./interfaces";

export const TeleprompterNode = defineNode({
  type: type_e.teleprompter,
  title: "Teleprompter",
  inputs: {
    fb_in: () => new NodeInterface<null>("FB In", null).use(setType, t_framebuffer),
    scroll_pos: () => new NumberInterface("Scroll Pos", 0).use(setType, t_f64),
    rect: () => new NodeInterface<null>("Rect", null).use(setType, t_rect),
    file_path: () => new TextInputInterface("File Path", "").setPort(false),
    font_name: () => new StatusDropdownInterface("Font", "font_names", "Liberation Sans"),
    font_variant: () => new StatusDropdownInterface("Variant", "font_variants", "Regular"),
    font_size: () => new FocusTrackingNumberInterface("Font Size", 100),
  },
  outputs: {
    fb_out: () => new NodeInterface<null>("FB Out", null).use(setType, t_framebuffer),
  },
});
