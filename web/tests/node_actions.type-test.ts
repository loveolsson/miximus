// Compiled by vue-tsc as part of npm run build; never executed.
import { requestNodeAction, type NodeActionCommand } from "../src/node_actions";
import { NodeActionInterface } from "../src/nodes/interfaces";
import { action_e } from "../src/messages";
import type { ws_wrapper } from "../src/websocket";

export async function checkActionContracts(ws: ws_wrapper) {
  const response = await requestNodeAction(ws, "cef_browser", "browser", { action: "reload" });
  if (response.action !== action_e.error) {
    const result: null = response.data;
    const roundTrip: typeof response.data = result;
    void roundTrip;
  }
  requestNodeAction(ws, "cef_browser", "browser", {
    action: "reload",
    payload: { ignore_cache: true, future_field: 42 },
  });
  requestNodeAction(ws, "application_settings", "$app", {
    action: "clear_browser_cache",
    payload: { future_field: true },
  });
  // @ts-expect-error Wrong action for the selected node type.
  requestNodeAction(ws, "cef_browser", "browser", { action: "clear_browser_cache" });
  requestNodeAction(ws, "cef_browser", "browser", {
    action: "reload",
    // @ts-expect-error Known fields must have the correct type.
    payload: { ignore_cache: "yes" },
  });
  requestNodeAction(ws, "cef_browser", "browser", {
    action: "reload",
    // @ts-expect-error A defaulted field is not nullable.
    payload: { ignore_cache: null },
  });
  requestNodeAction(ws, "application_settings", "$app", {
    action: "clear_browser_cache",
    // @ts-expect-error An empty payload schema still requires an object.
    payload: 123,
  });
  const array: NodeActionCommand<"application_settings"> = {
    action: "clear_browser_cache",
    // @ts-expect-error Arrays are not object payloads.
    payload: [],
  };
  void array;
  new NodeActionInterface("Reload", "cef_browser", [{ label: "Reload", action: "reload" }]);
  new NodeActionInterface("Reload", "cef_browser", [
    // @ts-expect-error Button action names are scoped to their node type.
    { label: "Clear", action: "clear_browser_cache" },
  ]);
  new NodeActionInterface("Reload", "cef_browser", [
    // @ts-expect-error Buttons preserve known payload field types.
    { label: "Reload", action: "reload", payload: { ignore_cache: 1 } },
  ]);
}
