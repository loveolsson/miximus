import { reactive } from "vue";
import type { node_status_s } from "@/generated/json_contracts";

export type { node_status_s } from "@/generated/json_contracts";

const store = reactive({
  statuses: {} as Record<string, node_status_s>,
});

export function update_node_status(node_id: string, status: node_status_s): void {
  store.statuses[node_id] = {
    ...(store.statuses[node_id] ?? {}),
    ...status,
  };
}

export function remove_node_status(node_id: string): void {
  delete store.statuses[node_id];
}

export function get_node_status(node_id: string): node_status_s {
  return store.statuses[node_id] ?? {};
}

export function clear_all_status(): void {
  for (const key of Object.keys(store.statuses)) {
    delete store.statuses[key];
  }
}

/** Reactive link between a node instance and its status-aware interface components. */
export interface NodeData {
  node_id: string;
}
