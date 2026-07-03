import { reactive } from "vue";

export type NodeStatus = Record<string, unknown>;

const store = reactive({
  statuses: {} as Record<string, NodeStatus>,
});

export function update_node_status(node_id: string, status: NodeStatus): void {
  store.statuses[node_id] = status;
}

export function remove_node_status(node_id: string): void {
  delete store.statuses[node_id];
}

export function get_node_status(node_id: string): NodeStatus {
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
