import { reactive } from "vue";
import type { Reactive } from "vue";
import { action_e, topic_e, type command_update_node_s, type options_s } from "./messages";
import { ApplicationSettingsNode } from "./nodes/system";
import type { ws_wrapper } from "./websocket";

export const SETTINGS_NODE_ID = "$app";
export type ApplicationSettingsView = Reactive<InstanceType<typeof ApplicationSettingsNode>>;
export type ApplicationSettingsInterface =
  ApplicationSettingsView["inputs"][keyof ApplicationSettingsView["inputs"]];

export function useApplicationSettings(ws: ws_wrapper) {
  const node: ApplicationSettingsView = reactive(new ApplicationSettingsNode());
  node.id = SETTINGS_NODE_ID;

  const eventToken = Symbol("application_settings");
  const serverUpdates = new Set<ApplicationSettingsInterface>();

  for (const [key, intf] of Object.entries(node.inputs)) {
    intf.events.setValue.subscribe(eventToken, (value) => {
      if (serverUpdates.has(intf)) return;
      ws.send<command_update_node_s>({
        action: action_e.command,
        topic: topic_e.update_node,
        id: SETTINGS_NODE_ID,
        options: { [key]: value },
      });
    });
  }

  function apply(options: options_s): void {
    const inputs = node.inputs as Record<string, ApplicationSettingsInterface>;
    for (const [key, value] of Object.entries(options)) {
      const intf = inputs[key];
      if (!intf) continue;

      serverUpdates.add(intf);
      try {
        intf.value = value as typeof intf.value;
      } finally {
        serverUpdates.delete(intf);
      }
    }
  }

  function destroy(): void {
    for (const intf of Object.values(node.inputs)) {
      intf.events.setValue.unsubscribe(eventToken);
    }
  }

  return { node, apply, destroy };
}
