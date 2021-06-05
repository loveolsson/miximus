<style scoped>
div.header {
  display: inline-block;
  flex: 0;
  background-color: #232323;
  border-bottom: 1px solid rgba(63, 63, 63, 0.8);
  filter: drop-shadow(0 0 3px rgba(0, 0, 0, 0.8));
  z-index: 1000;
  display: flex;
}

div.header div.appinfo {
  /* flex: 0; */
  display: flex;
  flex-direction: column;
  justify-content: center;
  height: 100%;
  vertical-align: middle;
}

div.header div.appinfo sup {
  vertical-align: super;
  font-size: 0.6em;
  color: #5379b5;
}

div.header .title {
  flex: 0;
  display: inline-block;
  color: #ffffff;
  font-size: 1.2em;
  font-weight: bold;
  padding-left: 1em;
}

div.header .title .offline {
  color: #bb0000;
  font-size: 1.2em;
  font-weight: 600;
  font-style: italic;
  padding-left: 1em;
}

div.header .controls {
  flex: 1;
  text-align: right;
}

div.header button {
  margin: 0.5em;
  background-color: rgba(63, 63, 63, 0.8);
  border: 0;
  color: #ffffff;
  border-radius: 0.5em;
  padding: 0.5em 1em;
  filter: drop-shadow(0 0 3px rgba(0, 0, 0, 0.8));
  transition: box-shadow 0.1s linear, filter 0.1s linear;
}

div.header button:hover {
  box-shadow: 0 0 0 1px #5379b5;
  filter: drop-shadow(0 0 7px rgba(0, 0, 0, 0.8));
}
</style>

<template>
  <div
    style="height: 100vh; width: 100vw; display: flex; flex-direction: column"
  >
    <div class="header">
      <div class="appinfo">
        <span class="title"
          >Miximus <sup>v0.0.1</sup>
          <span class="offline" v-if="!connected">Offline</span></span
        >
      </div>
      <div class="controls">
        <button v-on:click="save">Save</button>
        <button v-on:click="load">Load</button>
      </div>
    </div>
    <div style="flex: 1">
      <baklava-editor :plugin="viewPlugin" />
    </div>
  </div>
</template>

<script lang="ts">
import { Vue, Component } from "vue-property-decorator";
import { Editor, Node, Connection, NodeInterface } from "@baklavajs/core";
import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import { OptionPlugin } from "@baklavajs/plugin-options-vue";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { ws_wrapper } from "./websocket";
import { register_types } from "./nodes/register_types";
import { view_intercept } from "./view_intercept";
import { helpers } from "./helpers";
import {
  action_t,
  command_add_connection_s,
  command_add_node_s,
  command_config_s,
  command_remove_connection_s,
  command_remove_node_s,
  result_s,
  result_config_s,
  topic_t,
  connection_s,
} from "./messages";

@Component
export default class Miximus extends Vue {
  editor = new Editor();
  viewPlugin = new ViewPlugin();
  intfTypePlugin = new InterfaceTypePlugin();
  wsWrapper = new ws_wrapper();
  view_intercept = new view_intercept(this.viewPlugin, this.wsWrapper);
  nodes_mutated = true;
  node_to_be_added?: Node;
  node_to_be_removed?: Node;
  connection_to_be_added?: [NodeInterface, NodeInterface];
  connection_to_be_removed?: Connection;
  connections = new Set<Connection>();
  connected = false;

  created() {
    this.editor.use(this.viewPlugin);
    this.editor.use(this.intfTypePlugin);
    this.editor.use(new OptionPlugin());

    // Show a minimap in the top right corner
    this.viewPlugin.enableMinimap = true;

    register_types(this.editor);

    const token = {};
    this.editor.events.addNode.addListener(token, (ev) =>
      this.intercept_node_add(ev as Node)
    );
    this.editor.events.beforeRemoveNode.addListener(token, (ev) =>
      this.intercept_node_remove(ev as Node)
    );
    this.editor.events.beforeAddConnection.addListener(token, (ev) =>
      this.intercept_connection_add(ev as Connection)
    );
    this.editor.events.beforeRemoveConnection.addListener(token, (ev) =>
      this.intercept_connection_remove(ev as Connection)
    );

    this.wsWrapper.on("on_connected", this.handle_connected.bind(this));
    this.wsWrapper.on("on_disconnected", this.handle_disconnected.bind(this));

    this.wsWrapper.subscribe<command_add_node_s>(
      topic_t.add_node,
      (msg, is_origin) => {
        if (msg.action === action_t.command && msg.topic === topic_t.add_node) {
          this.handle_server_add_node(
            msg.node.type,
            msg.node.id,
            msg.node.options?.position,
            is_origin
          );
        }
      }
    );

    this.wsWrapper.subscribe<command_remove_node_s>(
      topic_t.remove_node,
      (msg) => {
        if (
          msg.action === action_t.command &&
          msg.topic === topic_t.remove_node
        ) {
          this.handle_server_remove_node(msg.id);
        }
      }
    );

    this.wsWrapper.subscribe<command_add_connection_s>(
      topic_t.add_connection,
      (msg) => {
        if (
          msg.action === action_t.command &&
          msg.topic === topic_t.add_connection
        ) {
          this.handle_server_add_connection(msg.connection);
        }
      }
    );

    this.wsWrapper.subscribe<command_remove_connection_s>(
      topic_t.remove_connection,
      (msg) => {
        if (
          msg.action === action_t.command &&
          msg.topic === topic_t.remove_connection
        ) {
          this.handle_server_remove_connection(msg.connection);
        }
      }
    );
  }

  destroyed() {
    this.wsWrapper.destroy();
  }

  handle_connected(id: number) {
    this.connected = true;

    const payload: command_config_s = {
      action: action_t.command,
      topic: topic_t.config,
    };

    this.wsWrapper.send<command_config_s, result_config_s>(payload, (msg) => {
      if (msg.action === action_t.result) {
        console.log("got config:", msg);

        for (const node of msg.config.nodes) {
          this.handle_server_add_node(
            node.type,
            node.id,
            node.options?.position,
            false
          );

          if (node.options) {
            this.view_intercept.handle_server_update_node(
              node.id,
              node.options
            );
          }
        }

        for (const con of msg.config.connections) {
          this.handle_server_add_connection(con);
        }
      }
    });
  }

  handle_disconnected(code: number, reason: string) {
    console.log("WebSocket disconnected: ", code, reason);

    this.connected = false;

    for (const con of this.editor.connections) {
      this.connection_to_be_removed = con;
      this.editor.removeConnection(con);
    }

    for (const node of this.editor.nodes) {
      this.node_to_be_removed = node;
      this.editor.removeNode(node);
    }
  }

  handle_server_add_node(
    type: string,
    id: string,
    position: [number, number] | undefined,
    is_origin: boolean
  ) {
    if (is_origin) {
      return;
    }

    const node_type = this.editor.nodeTypes.get(type);
    if (!node_type) {
      return console.error(`Node type ${type} not found`);
    }

    const node = new node_type() as Node;
    node.id = id;

    if (position) {
      (node as any).position = {
        x: position[0],
        y: position[1],
      };
    }

    this.node_to_be_added = node;
    this.editor.addNode(node);
  }

  handle_server_remove_node(id: string) {
    const node = helpers.find_node(this.editor, id);
    if (!node) {
      return console.error(`Node ${id} not found`);
    }

    this.node_to_be_removed = node;
    this.editor.removeNode(node);
  }

  handle_server_add_connection(con: connection_s) {
    const from = helpers.find_interface(
      this.editor,
      con.from_node,
      con.from_interface
    );
    const to = helpers.find_interface(
      this.editor,
      con.to_node,
      con.to_interface
    );
    if (!from || !to) {
      return console.error(
        `Trying to add a connection between interfaces that does not exist`
      );
    }

    this.connection_to_be_added = [from, to];
    const connection = this.editor.addConnection(from, to);
    if (connection) {
      this.connections.add(connection);
    }
  }

  handle_server_remove_connection(con: connection_s) {
    const from = helpers.find_interface(
      this.editor,
      con.from_node,
      con.from_interface
    );
    const to = helpers.find_interface(
      this.editor,
      con.to_node,
      con.to_interface
    );
    if (!from || !to) {
      return console.error(
        `Trying to remove a connection between interfaces that does not exist`
      );
    }

    const connection = helpers.find_connection(this.connections, from, to);
    if (connection) {
      this.connection_to_be_removed = connection;
      this.editor.removeConnection(connection);
    }
  }

  intercept_node_add(node: Node): boolean {
    if (this.node_to_be_added === node) {
      this.node_to_be_added = undefined;
      return true;
    }

    // This needs to be put on a timer, otherwise the position is not set
    setTimeout(() => {
      const position = (node as any).position;

      const payload: command_add_node_s = {
        action: action_t.command,
        topic: topic_t.add_node,
        node: {
          type: node.type,
          id: node.id,
          options: {
            position: [position.x, position.y],
          },
        },
      };

      this.wsWrapper.send<command_add_node_s, result_s>(payload, (msg) => {
        if (msg.action === action_t.error) {
          this.node_to_be_removed = node;
          this.editor.removeNode(node);
        }
      });
    }, 0);

    return false;
  }

  intercept_node_remove(node: Node): boolean {
    if (this.node_to_be_removed === node) {
      this.node_to_be_removed = undefined;
      this.view_intercept.remove(node.id);
      return true;
    }

    const payload: command_remove_node_s = {
      action: action_t.command,
      topic: topic_t.remove_node,
      id: node.id,
    };

    this.wsWrapper.send(payload);

    return false;
  }

  intercept_connection_add(con: Connection): boolean {
    const tba = this.connection_to_be_added;
    if (tba && tba[0] === con.from && tba[1] === con.to) {
      this.connection_to_be_added = undefined;
      return true;
    }

    const from_interface = helpers.get_interface_name(con.from);
    const to_interface = helpers.get_interface_name(con.to);

    if (!from_interface || !to_interface) return true;

    const payload: command_add_connection_s = {
      action: action_t.command,
      topic: topic_t.add_connection,
      connection: {
        from_node: con.from.parent.id,
        from_interface,
        to_node: con.to.parent.id,
        to_interface,
      },
    };

    this.wsWrapper.send(payload);

    return false;
  }

  intercept_connection_remove(con: Connection): boolean {
    if (this.connection_to_be_removed === con) {
      this.connection_to_be_removed = undefined;
      this.connections.delete(con);
      return true;
    }

    const from_interface = helpers.get_interface_name(con.from);
    const to_interface = helpers.get_interface_name(con.to);
    if (!from_interface || !to_interface) {
      return false;
    }

    const payload: command_remove_connection_s = {
      action: action_t.command,
      topic: topic_t.remove_connection,
      connection: {
        from_node: con.from.parent.id,
        from_interface,
        to_node: con.to.parent.id,
        to_interface,
      },
    };

    this.wsWrapper.send(payload);

    return false;
  }

  save() {
    console.log("Saving", JSON.stringify(this.editor.save(), null, 2));
  }

  load() {
    console.log("Loading");
  }
}
</script>
