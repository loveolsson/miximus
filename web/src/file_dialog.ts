import { action_e, topic_e, type file_dialog_request_s, type result_s } from "./messages";
import type { ws_wrapper } from "./websocket";

// Acknowledges admission only. The selected path arrives through normal broadcasts.
export function requestFileDialog(ws: ws_wrapper, nodeId: string, signal?: AbortSignal) {
  return ws.request<file_dialog_request_s, result_s>(
    { action: action_e.command, topic: topic_e.file_dialog, id: nodeId },
    signal,
  );
}
