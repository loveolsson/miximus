<template>
  <div class="app-root">
    <div class="header">
      <span class="title">
        Miximus <sup>v0.0.1</sup>
        <span v-if="!connected" class="offline">&nbsp;— offline</span>
      </span>
    </div>
    <div class="editor-area">
      <BaklavaEditor :view-model="baklava">
        <template #connection="{ connection }">
          <ColoredConnectionWrapper :connection="connection" />
        </template>
      </BaklavaEditor>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, nextTick, onUnmounted, provide } from "vue";
import {
  BaklavaEditor,
  useBaklava,
  TOOLBAR_COMMANDS,
  ZOOM_TO_FIT_GRAPH_COMMAND,
} from "@baklavajs/renderer-vue";
import "@baklavajs/themes/dist/syrup-dark.css";
import ColoredConnectionWrapper from "./components/ColoredConnectionWrapper.vue";

import { websocket_key, ws_wrapper } from "./websocket";
import { register_node_types, register_interface_types } from "./nodes/types";
import { update_node_status, clear_all_status } from "./nodes/status_store";
import { useServerSync } from "./server_sync";
import {
  action_e,
  topic_e,
  type command_add_node_s,
  type command_remove_node_s,
  type command_update_node_s,
  type command_add_connection_s,
  type command_remove_connection_s,
  type command_config_s,
  type command_node_status_s,
  type result_config_s,
} from "./messages";

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

const baklava = useBaklava();
const ws = new ws_wrapper();
provide(websocket_key, ws);

register_node_types(baklava.editor);
register_interface_types(baklava);

// Remove toolbar items that are not meaningful in a server-driven graph.
const _excludedToolbarCommands = new Set([
  TOOLBAR_COMMANDS.UNDO.command,
  TOOLBAR_COMMANDS.REDO.command,
  TOOLBAR_COMMANDS.COPY.command,
  TOOLBAR_COMMANDS.PASTE.command,
  TOOLBAR_COMMANDS.CREATE_SUBGRAPH.command,
]);
baklava.settings.toolbar.commands = baklava.settings.toolbar.commands.filter(
  (c) => !_excludedToolbarCommands.has(c.command),
);

const {
  handle_server_add_node,
  handle_server_remove_node,
  handle_server_update_node,
  handle_server_add_connection,
  handle_server_remove_connection,
  handle_server_init_node_status,
  destroy: destroySync,
} = useServerSync(baklava, ws);

const connected = ref(false);

// ---------------------------------------------------------------------------
// WebSocket subscriptions
// ---------------------------------------------------------------------------

// --- add_node ---
ws.subscribe<command_add_node_s>(topic_e.add_node, (msg, is_origin) => {
  if (msg.action !== action_e.command) return;
  if (is_origin) {
    // Our own add was acknowledged; server may have filled in options.
    handle_server_update_node(msg.node.id, msg.node.options);
    return;
  }
  handle_server_add_node(msg.node.type, msg.node.id);
  handle_server_update_node(msg.node.id, msg.node.options);
});

// --- remove_node ---
ws.subscribe<command_remove_node_s>(topic_e.remove_node, (msg, is_origin) => {
  if (msg.action !== action_e.command) return;
  if (is_origin) return;
  handle_server_remove_node(msg.id);
});

// --- update_node ---
ws.subscribe<command_update_node_s>(topic_e.update_node, (msg, is_origin) => {
  if (msg.action !== action_e.command) return;
  if (is_origin) return;
  handle_server_update_node(msg.id, msg.options);
});

// --- add_connection ---
ws.subscribe<command_add_connection_s>(topic_e.add_connection, (msg, is_origin) => {
  if (msg.action !== action_e.command) return;
  if (is_origin) return;
  handle_server_add_connection(msg.connection);
});

// --- remove_connection ---
// NOTE: do NOT guard on is_origin here. When the client adds a connection that
// displaces an existing one, the server auto-removes the old connection and
// broadcasts remove_connection with origin_id = this client. We must process
// that side-effect. handle_server_remove_connection is idempotent (returns
// early when the connection is already gone), so echoes of our own explicit
// removals are also safe.
ws.subscribe<command_remove_connection_s>(topic_e.remove_connection, (msg, _is_origin) => {
  if (msg.action !== action_e.command) return;
  handle_server_remove_connection(msg.connection);
});

// --- node_status (push broadcasts) ---
ws.subscribe<command_node_status_s>(topic_e.node_status, (msg) => {
  if (msg.action !== action_e.command) return;
  update_node_status(msg.id, msg.status);
  handle_server_init_node_status(msg.id);
});

// --- on_connected: request config via one-shot send (action:result response) ---
ws.on("on_connected", () => {
  const payload: command_config_s = {
    action: action_e.command,
    topic: topic_e.config,
  };

  ws.send<command_config_s, result_config_s>(payload, (msg) => {
    if (msg.action !== action_e.result) return;
    const config = (msg as result_config_s).config;

    // Clear existing graph first.
    for (const node of [...baklava.editor.graph.nodes]) {
      handle_server_remove_node(node.id);
    }

    // Restore persisted status.
    if (config.status) {
      for (const [id, status] of Object.entries(config.status)) {
        update_node_status(id, status);
      }
    }

    for (const node of config.nodes) {
      handle_server_add_node(node.type, node.id);
      handle_server_update_node(node.id, node.options);
    }

    for (const con of config.connections) {
      handle_server_add_connection(con);
    }

    // Push node_id into status-aware interfaces after graph is built.
    for (const node of baklava.editor.graph.nodes) {
      handle_server_init_node_status(node.id);
    }

    connected.value = true;
    nextTick(() => baklava.commandHandler.executeCommand(ZOOM_TO_FIT_GRAPH_COMMAND));
  });
});

// --- on_disconnected: clear graph and status ---
ws.on("on_disconnected", () => {
  connected.value = false;
  for (const node of [...baklava.editor.graph.nodes]) {
    handle_server_remove_node(node.id);
  }
  clear_all_status();
});

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

onUnmounted(() => {
  destroySync();
  ws.destroy();
});
</script>

<style>
/* Minimal global resets so the editor fills the viewport */
html,
body,
#app {
  margin: 0;
  padding: 0;
  width: 100%;
  height: 100%;
  overflow: hidden;
}

/* Color port dots by interface type (data-interface-type is set by BaklavaInterfaceTypes) */
.baklava-node-interface[data-interface-type="texture"] {
  --baklava-node-interface-port-color: #332288;
}
.baklava-node-interface[data-interface-type="framebuffer"] {
  --baklava-node-interface-port-color: #88ccee;
}
.baklava-node-interface[data-interface-type="f64"] {
  --baklava-node-interface-port-color: #117733;
}
.baklava-node-interface[data-interface-type="vec2"] {
  --baklava-node-interface-port-color: #999933;
}
.baklava-node-interface[data-interface-type="rect"] {
  --baklava-node-interface-port-color: #cc6677;
}
</style>

<style scoped>
.app-root {
  display: flex;
  flex-direction: column;
  width: 100vw;
  height: 100vh;
  background: #0e0e16;
  color: #e0e0e0;
}

.header {
  display: flex;
  align-items: center;
  padding: 4px 12px;
  background: #16162a;
  border-bottom: 1px solid #2a2a4a;
  flex-shrink: 0;
  gap: 12px;
  font-size: 0.9em;
}

.title {
  font-weight: 600;
}
.title sup {
  font-size: 0.65em;
  color: #777;
}
.offline {
  color: #cc5555;
  font-weight: normal;
  font-size: 0.9em;
}

.editor-area {
  flex: 1;
  overflow: hidden;
  position: relative;
}
</style>
