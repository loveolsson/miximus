import Vue from "vue";

export type NodeStatus = Record<string, unknown>;

/**
 * Global reactive store for node status.
 * Updated by node_status broadcasts from the server and pre-populated
 * from the initial config response.
 */
const store = Vue.observable({
  statuses: {} as Record<string, NodeStatus>,
});

export function update_node_status(node_id: string, status: NodeStatus): void {
  Vue.set(store.statuses, node_id, status);
}

export function remove_node_status(node_id: string): void {
  Vue.delete(store.statuses, node_id);
}

export function get_node_status(node_id: string): NodeStatus {
  return store.statuses[node_id] ?? {};
}

export function clear_all_status(): void {
  for (const id of Object.keys(store.statuses)) {
    Vue.delete(store.statuses, id);
  }
}

/**
 * Reactive data object shared between a node instance and its status-aware
 * option components. Created per node, updated with the server-assigned ID
 * after the node is added to the editor.
 */
export interface NodeOptionData {
  node_id: string;
}

export function make_node_option_data(): NodeOptionData {
  return Vue.observable({ node_id: "" });
}
