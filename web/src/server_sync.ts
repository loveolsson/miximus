/** Bridges BaklavaJS graph events with the authoritative server graph. */

import { watch } from "vue";
import type { IBaklavaViewModel } from "@baklavajs/renderer-vue";
import type { AbstractNode, Graph, NodeInterface } from "@baklavajs/core";

import { ws_wrapper } from "./websocket";
import {
  action_e,
  topic_e,
  type_e,
  type command_add_connection_s,
  type command_add_node_s,
  type command_remove_connection_s,
  type command_remove_node_s,
  type command_update_node_s,
  type connection_s,
  type options_s,
  type position_t,
} from "./messages";
import {
  find_connection,
  find_interface,
  find_node,
  find_node_and_key_for_interface,
} from "./helpers";
import { has_node_data } from "./nodes/interfaces";

const POSITION_DEBOUNCE_MS = 250;
const CONNECTION_MUTATION_KEY = Symbol("connection_mutation");

type mutation_kind_t =
  | "add_node"
  | "remove_node"
  | "set_value"
  | "set_position"
  | "set_title"
  | "add_connection"
  | "remove_connection";

/**
 * Baklava emits graph and interface changes synchronously. Track mutations
 * applied from the server so their corresponding event handlers do not echo
 * them back. Counts make nested mutations safe, and finally prevents a thrown
 * Baklava callback from leaving suppression enabled.
 */
class server_mutation_guard_s {
  private readonly active = new Map<mutation_kind_t, Map<unknown, number>>();

  public contains(kind: mutation_kind_t, key: unknown): boolean {
    return (this.active.get(kind)?.get(key) ?? 0) !== 0;
  }

  public run<T>(kind: mutation_kind_t, key: unknown, mutate: () => T): T {
    const mutations = this.active.get(kind) ?? new Map<unknown, number>();
    mutations.set(key, (mutations.get(key) ?? 0) + 1);
    this.active.set(kind, mutations);

    try {
      return mutate();
    } finally {
      const remaining = (mutations.get(key) ?? 1) - 1;
      if (remaining === 0) {
        mutations.delete(key);
        if (mutations.size === 0) this.active.delete(kind);
      } else {
        mutations.set(key, remaining);
      }
    }
  }
}

function connection_payload(
  graph: Graph,
  from: NodeInterface,
  to: NodeInterface,
): connection_s | undefined {
  const fromResult = find_node_and_key_for_interface(graph, from);
  const toResult = find_node_and_key_for_interface(graph, to);
  if (!fromResult || !toResult) return undefined;

  return {
    from_node: fromResult[0].id,
    from_interface: fromResult[1],
    to_node: toResult[0].id,
    to_interface: toResult[1],
  };
}

function assign_node_id(node: AbstractNode): void {
  for (const intf of Object.values(node.inputs)) {
    if (has_node_data(intf)) intf.nodeData.node_id = node.id;
  }
}

export function useServerSync(baklava: IBaklavaViewModel, ws: ws_wrapper) {
  const graph: Graph = baklava.editor.graph;
  const eventToken = Symbol("server_sync");
  const serverMutations = new server_mutation_guard_s();
  const positionTimers = new Map<string, ReturnType<typeof setTimeout>>();
  const nodeCleanups = new Map<string, () => void>();

  function clearPositionTimer(nodeId: string): void {
    const timer = positionTimers.get(nodeId);
    if (timer !== undefined) clearTimeout(timer);
    positionTimers.delete(nodeId);
  }

  function debouncePosition(nodeId: string, position: position_t): void {
    clearPositionTimer(nodeId);
    positionTimers.set(
      nodeId,
      setTimeout(() => {
        positionTimers.delete(nodeId);
        ws.send<command_update_node_s>({
          action: action_e.command,
          topic: topic_e.update_node,
          id: nodeId,
          options: { node_visual_position: position },
        });
      }, POSITION_DEBOUNCE_MS),
    );
  }

  function subscribeNode(node: AbstractNode): void {
    const nodeToken = Symbol(node.id);
    const stopWatching: Array<() => void> = [];

    for (const [key, intf] of Object.entries(node.inputs)) {
      intf.events.setValue.subscribe(nodeToken, (value) => {
        if (serverMutations.contains("set_value", intf)) return;
        ws.send<command_update_node_s>({
          action: action_e.command,
          topic: topic_e.update_node,
          id: node.id,
          options: { [key]: value },
        });
      });
    }

    stopWatching.push(
      watch(
        () => node.position,
        (position) => {
          if (!position || serverMutations.contains("set_position", node.id)) return;
          debouncePosition(node.id, [position.x, position.y]);
        },
        { deep: true, flush: "sync" },
      ),
    );

    stopWatching.push(
      watch(
        () => node.title,
        (title) => {
          if (serverMutations.contains("set_title", node.id)) return;
          ws.send<command_update_node_s>({
            action: action_e.command,
            topic: topic_e.update_node,
            id: node.id,
            options: { name: title },
          });
        },
        { flush: "sync" },
      ),
    );

    nodeCleanups.set(node.id, () => {
      for (const intf of Object.values(node.inputs)) {
        intf.events.setValue.unsubscribe(nodeToken);
      }
      for (const stop of stopWatching) stop();
      clearPositionTimer(node.id);
    });
  }

  function removeNodeSubscriptions(nodeId: string): void {
    nodeCleanups.get(nodeId)?.();
    nodeCleanups.delete(nodeId);
  }

  function sendAddNode(node: AbstractNode): void {
    const position = node.position;
    ws.send<command_add_node_s>(
      {
        action: action_e.command,
        topic: topic_e.add_node,
        node: {
          type: node.type as type_e,
          id: node.id,
          options: { node_visual_position: position ? [position.x, position.y] : [0, 0] },
        },
      },
      (response) => {
        if (response.action !== action_e.error) return;
        serverMutations.run("remove_node", node.id, () => graph.removeNode(node));
      },
    );
  }

  // Client-originated graph changes.
  graph.events.addNode.subscribe(eventToken, (node) => {
    subscribeNode(node);
    if (!serverMutations.contains("add_node", node.id)) {
      // Baklava finishes wrapping a new node in its reactive proxy before this
      // event. Defer the command until that local add has fully unwound.
      setTimeout(() => sendAddNode(node), 0);
    }
  });

  graph.events.removeNode.subscribe(eventToken, (node) => {
    removeNodeSubscriptions(node.id);
    if (!serverMutations.contains("remove_node", node.id)) {
      ws.send<command_remove_node_s>({
        action: action_e.command,
        topic: topic_e.remove_node,
        id: node.id,
      });
    }
  });

  graph.events.addConnection.subscribe(eventToken, (connection) => {
    if (serverMutations.contains("add_connection", CONNECTION_MUTATION_KEY)) return;
    const payload = connection_payload(graph, connection.from, connection.to);
    if (!payload) return;

    ws.send<command_add_connection_s>(
      {
        action: action_e.command,
        topic: topic_e.add_connection,
        connection: payload,
      },
      (response) => {
        if (response.action !== action_e.error) return;
        const existing = find_connection(graph, connection.from, connection.to);
        if (!existing) return;
        serverMutations.run("remove_connection", CONNECTION_MUTATION_KEY, () =>
          graph.removeConnection(existing),
        );
      },
    );
  });

  graph.events.removeConnection.subscribe(eventToken, (connection) => {
    if (serverMutations.contains("remove_connection", CONNECTION_MUTATION_KEY)) return;
    const payload = connection_payload(graph, connection.from, connection.to);
    if (!payload) return;

    ws.send<command_remove_connection_s>({
      action: action_e.command,
      topic: topic_e.remove_connection,
      connection: payload,
    });
  });

  // Server-originated graph changes.
  function handle_server_add_node(type: string, id: string): void {
    const info = baklava.editor.nodeTypes.get(type);
    if (!info) {
      console.warn(`[server_sync] Unknown node type: ${type}`);
      return;
    }

    const node = new info.type();
    node.id = id;
    assign_node_id(node);
    serverMutations.run("add_node", id, () => graph.addNode(node));
  }

  function handle_server_remove_node(id: string): void {
    const node = find_node(graph, id);
    if (!node) return;
    serverMutations.run("remove_node", id, () => graph.removeNode(node));
  }

  function handle_server_update_node(id: string, options: options_s): void {
    const node = find_node(graph, id);
    if (!node) return;

    for (const [key, value] of Object.entries(options)) {
      if (key === "node_visual_position" && Array.isArray(value)) {
        const [x, y] = value as [number, number];
        clearPositionTimer(id);
        serverMutations.run("set_position", id, () => {
          node.position = { x, y };
        });
        continue;
      }

      if (key === "name" && typeof value === "string") {
        serverMutations.run("set_title", id, () => {
          node.title = value;
        });
        continue;
      }

      const intf = node.inputs[key];
      if (intf) {
        serverMutations.run("set_value", intf, () => {
          intf.value = value as typeof intf.value;
        });
      }
    }
  }

  function handle_server_add_connection(connection: connection_s): void {
    const from = find_interface(graph, connection.from_node, connection.from_interface);
    const to = find_interface(graph, connection.to_node, connection.to_interface);
    if (!from || !to) return;
    serverMutations.run("add_connection", CONNECTION_MUTATION_KEY, () =>
      graph.addConnection(from, to),
    );
  }

  function handle_server_remove_connection(connection: connection_s): void {
    const from = find_interface(graph, connection.from_node, connection.from_interface);
    const to = find_interface(graph, connection.to_node, connection.to_interface);
    if (!from || !to) return;

    const existing = find_connection(graph, from, to);
    if (!existing) return;
    serverMutations.run("remove_connection", CONNECTION_MUTATION_KEY, () =>
      graph.removeConnection(existing),
    );
  }

  /** Push the node ID into interfaces that consume node status. */
  function handle_server_init_node_status(id: string): void {
    const node = find_node(graph, id);
    if (node) assign_node_id(node);
  }

  function destroy(): void {
    graph.events.addNode.unsubscribe(eventToken);
    graph.events.removeNode.unsubscribe(eventToken);
    graph.events.addConnection.unsubscribe(eventToken);
    graph.events.removeConnection.unsubscribe(eventToken);
    for (const cleanup of nodeCleanups.values()) cleanup();
    nodeCleanups.clear();
  }

  return {
    handle_server_add_node,
    handle_server_remove_node,
    handle_server_update_node,
    handle_server_add_connection,
    handle_server_remove_connection,
    handle_server_init_node_status,
    destroy,
  };
}
