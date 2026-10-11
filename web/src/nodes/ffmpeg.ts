import { defineNode, NodeInterface } from "@baklavajs/core";
import { CheckboxInterface } from "@baklavajs/renderer-vue";
import { setType } from "@baklavajs/interface-types";
import { DropdownInterface, FilePathInterface, NodeStatusInterface } from "./interfaces";
import { node_type_e } from "./node_type";
import { t_texture } from "./interface_types";

export const FfmpegPlaybackLoopingNode = defineNode({
  type: node_type_e.ffmpeg_playback_looping,
  title: "Video playback (looping)",
  inputs: {
    status: () =>
      new NodeStatusInterface([
        {
          title: "Playback",
          fields: [
            { key: "connected", label: "Ready" },
            { key: "playback_backend", label: "Decoder" },
            { key: "playback_transfer_path", label: "Frame transfer" },
            { key: "playback_fallback_reason", label: "Fallback reason" },
            { key: "playback_error", label: "Error" },
            { key: "playback_position_seconds", label: "Position (s)", precision: 2 },
            { key: "playback_queue_depth", label: "Buffered frames", format: "integer" },
            { key: "playback_decoded_frames", label: "Decoded frames", format: "integer" },
            { key: "playback_dropped_frames", label: "Late frames", format: "integer" },
            { key: "playback_loops", label: "Decoded loops", format: "integer" },
          ],
        },
      ]),
    enabled: () => new CheckboxInterface("Enabled", true).setPort(false),
    filename: () => new FilePathInterface("Filename"),
    playback_mode: () =>
      new DropdownInterface("Playback mode", "always_play", [
        { id: "always_play", label: "Always play" },
        { id: "pause_inactivity", label: "Pause inactivity" },
        { id: "pause_and_rewind_inactive", label: "Pause and rewind inactive" },
      ]),
    attempt_hardware_decode: () =>
      new CheckboxInterface("Attempt hardware decode", true).setPort(false),
  },
  outputs: {
    texture: () => new NodeInterface<null>("Texture", null).use(setType, t_texture),
  },
});
