/**
 * server_sync.ts — bridges BaklavaJS v2 graph events ↔ server WebSocket.
 *
 * Sentinel pattern:
 *   Server-initiated changes add the node/interface to a Set before mutating,
 *   and the event subscriber checks that Set to avoid echoing the change back.
 */

import { watch } from "vue";
import type { IBaklavaViewModel } from "@baklavajs/renderer-vue";
import type { AbstractNode, NodeInterface, Graph } from "@baklavajs/core";

import { ws_wrapper } from "./websocket";
import {
  action_e,
  topic_e,
  type_e,
  type command_add_node_s,
  type command_remove_node_s,
  type command_update_node_s,
  type command_add_connection_s,
  type command_remove_connection_s,
  type connection_s,
  type options_s,
  type position_t,
} from "./messages";
import {
  find_node,
  find_interface,
  find_node_and_key_for_interface,
  find_connection,
} from "./helpers";
import { has_node_data } from "./nodes/interfaces";

const POSITION_DEBOUNCE_MS = 250;

export function useServerSync(baklava: IBaklavaViewModel, ws: ws_wrapper) {
  const graph: Graph = baklava.editor.graph;
  const token = Symbol("server_sync");

  // ---- sentinel sets ----
  // NOTE: graph.addNode() replaces the original node with its Vue reactive
  // proxy before emitting the addNode event, so object identity cannot be
  // used. Use the node ID (string) instead.
  const serverAddedNodeIds = new Set<string>();
  const serverRemovedNodeIds = new Set<string>();
  const serverValueIntfs = new Set<NodeInterface>();
  const serverPositionIds = new Set<string>();
  const serverTitleIds = new Set<string>();
  let _addingServerConn = false;
  let _removingServerConn = false;

  // ---- cleanup tracking ----
  const positionTimers = new Map<string, ReturnType<typeof setTimeout>>();
  const nodeCleanups = new Map<string, () => void>();

  // ==========================================================================
  // Graph-level events
  // ==========================================================================

  graph.events.addNode.subscribe(token, (node) => {
    const isServer = serverAddedNodeIds.has(node.id);
    serverAddedNodeIds.delete(node.id);
    _subscribeNode(node);
    if (!isServer) {
      setTimeout(() => _sendAddNode(node), 0);
    }
  });

  graph.events.removeNode.subscribe(token, (node) => {
    const isServer = serverRemovedNodeIds.has(node.id);
    serverRemovedNodeIds.delete(node.id);
    nodeCleanups.get(node.id)?.();
    nodeCleanups.delete(node.id);
    if (!isServer) {
      ws.send<command_remove_node_s>({
        action: action_e.command,
        topic: topic_e.remove_node,
        id: node.id,
      });
    }
  });

  graph.events.addConnection.subscribe(token, (connection) => {
    if (_addingServerConn) return;
    const fromResult = find_node_and_key_for_interface(graph, connection.from);
    const toResult = find_node_and_key_for_interface(graph, connection.to);
    if (!fromResult || !toResult) return;
    ws.send<command_add_connection_s>(
      {
        action: action_e.command,
        topic: topic_e.add_connection,
        connection: {
          from_node: fromResult[0].id,
          from_interface: fromResult[1],
          to_node: toResult[0].id,
          to_interface: toResult[1],
        },
      },
      (response) => {
        if (response.action === action_e.error) {
          // Server rejected the connection — roll back the local add.
          // Use find_connection to obtain the concrete Connection instance
          // (the event emits IConnection; removeConnection requires Connection).
          const existing = find_connection(graph, connection.from, connection.to);
          if (existing) {
            _removingServerConn = true;
            graph.removeConnection(existing);
            _removingServerConn = false;
          }
        }
      },
    );
  });

  graph.events.removeConnection.subscribe(token, (connection) => {
    if (_removingServerConn) return;
    const fromResult = find_node_and_key_for_interface(graph, connection.from);
    const toResult = find_node_and_key_for_interface(graph, connection.to);
    if (!fromResult || !toResult) return;
    ws.send<command_remove_connection_s>({
      action: action_e.command,
      topic: topic_e.remove_connection,
      connection: {
        from_node: fromResult[0].id,
        from_interface: fromResult[1],
        to_node: toResult[0].id,
        to_interface: toResult[1],
      },
    });
  });

  // ==========================================================================
  // Per-node subscriptions
  // ==========================================================================

  function _subscribeNode(node: AbstractNode): void {
    const nodeToken = Symbol(node.id);
    const stoppers: Array<() => void> = [];

    for (const [key, intf] of Object.entries(node.inputs)) {
      intf.events.setValue.subscribe(nodeToken, (value) => {
        if (serverValueIntfs.has(intf)) return;
        ws.send<command_update_node_s>({
          action: action_e.command,
          topic: topic_e.update_node,
          id: node.id,
          options: { [key]: value },
        });
      });
    }

    // Position — flush:'sync' so sentinel delete is immediate.
    stoppers.push(
      watch(
        () => node.position,
        (pos) => {
          if (!pos) return;
          if (serverPositionIds.has(node.id)) return;
          _debouncePosition(node.id, [pos.x, pos.y]);
        },
        { deep: true, flush: "sync" },
      ),
    );

    // Title
    stoppers.push(
      watch(
        () => node.title,
        (title) => {
          if (serverTitleIds.has(node.id)) return;
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
      stoppers.forEach((s) => s());
      const timer = positionTimers.get(node.id);
      if (timer !== undefined) clearTimeout(timer);
      positionTimers.delete(node.id);
    });
  }

  function _debouncePosition(nodeId: string, pos: position_t): void {
    const existing = positionTimers.get(nodeId);
    if (existing !== undefined) clearTimeout(existing);
    positionTimers.set(
      nodeId,
      setTimeout(() => {
        positionTimers.delete(nodeId);
        ws.send<command_update_node_s>({
          action: action_e.command,
          topic: topic_e.update_node,
          id: nodeId,
          options: { node_visual_position: pos },
        });
      }, POSITION_DEBOUNCE_MS),
    );
  }

  function _sendAddNode(node: AbstractNode): void {
    const pos = node.position;
    ws.send<command_add_node_s>(
      {
        action: action_e.command,
        topic: topic_e.add_node,
        node: {
          type: node.type as type_e,
          id: node.id,
          options: { node_visual_position: pos ? [pos.x, pos.y] : [0, 0] },
        },
      },
      (response) => {
        if (response.action === action_e.error) {
          serverRemovedNodeIds.add(node.id);
          graph.removeNode(node);
        }
      },
    );
  }

  // ==========================================================================
  // Handlers for server-initiated changes
  // ==========================================================================

  function handle_server_add_node(type: string, id: string): void {
    const info = baklava.editor.nodeTypes.get(type);
    if (!info) {
      console.warn(`[server_sync] Unknown node type: ${type}`);
      return;
    }
    const node = new info.type();
    node.id = id;
    for (const intf of Object.values(node.inputs)) {
      if (has_node_data(intf)) intf.nodeData.node_id = id;
    }
    serverAddedNodeIds.add(id);
    graph.addNode(node);
  }

  function handle_server_remove_node(id: string): void {
    const node = find_node(graph, id);
    if (!node) return;
    serverRemovedNodeIds.add(id);
    graph.removeNode(node);
  }

  function handle_server_update_node(id: string, options: options_s): void {
    const node = find_node(graph, id);
    if (!node) return;
    for (const [key, value] of Object.entries(options)) {
      if (key === "node_visual_position" && Array.isArray(value)) {
        const [px, py] = value as [number, number];
        serverPositionIds.add(id);
        node.position = { x: px, y: py };
        serverPositionIds.delete(id);
      } else if (key === "name" && typeof value === "string") {
        serverTitleIds.add(id);
        node.title = value;
        serverTitleIds.delete(id);
      } else {
        const intf = node.inputs[key];
        if (intf) {
          serverValueIntfs.add(intf);
          intf.value = value as typeof intf.value;
          serverValueIntfs.delete(intf);
        }
      }
    }
  }

  function handle_server_add_connection(con: connection_s): void {
    const fromIntf = find_interface(graph, con.from_node, con.from_interface);
    const toIntf = find_interface(graph, con.to_node, con.to_interface);
    if (!fromIntf || !toIntf) return;
    _addingServerConn = true;
    graph.addConnection(fromIntf, toIntf);
    _addingServerConn = false;
  }

  function handle_server_remove_connection(con: connection_s): void {
    const fromIntf = find_interface(graph, con.from_node, con.from_interface);
    const toIntf = find_interface(graph, con.to_node, con.to_interface);
    if (!fromIntf || !toIntf) return;
    const connection = find_connection(graph, fromIntf, toIntf);
    if (!connection) return;
    _removingServerConn = true;
    graph.removeConnection(connection);
    _removingServerConn = false;
  }

  /** Called after a node_status update to push node_id into status-aware interfaces. */
  function handle_server_init_node_status(id: string): void {
    const node = find_node(graph, id);
    if (!node) return;
    for (const intf of Object.values(node.inputs)) {
      if (has_node_data(intf)) intf.nodeData.node_id = id;
    }
  }

  // ==========================================================================
  // Cleanup
  // ==========================================================================

  function destroy(): void {
    graph.events.addNode.unsubscribe(token);
    graph.events.removeNode.unsubscribe(token);
    graph.events.addConnection.unsubscribe(token);
    graph.events.removeConnection.unsubscribe(token);
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
