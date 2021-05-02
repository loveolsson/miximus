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
import { Editor, Node, Connection } from "@baklavajs/core";
import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import { OptionPlugin } from "@baklavajs/plugin-options-vue";
import { InterfaceTypePlugin } from "@baklavajs/plugin-interface-types";
import { action_t, message, topic_t, ws_wrapper } from "./websocket";
import { register_types } from "./nodes/register_types";
import { Positions } from "./positions";

@Component
export default class Miximus extends Vue {
  editor = new Editor();
  viewPlugin = new ViewPlugin();
  intfTypePlugin = new InterfaceTypePlugin();
  wsWrapper = new ws_wrapper();
  positions = new Positions(this.viewPlugin, this.wsWrapper);
  nodes_mutated = true;
  nodes_to_be_removed = new Set<Node>();
  connections_to_be_removed = new Set<Connection>();
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
      this.handle_node_add(ev as Node)
    );
    this.editor.events.beforeRemoveNode.addListener(token, (ev) =>
      this.handle_node_remove(ev as Node)
    );
    this.editor.events.beforeAddConnection.addListener(token, (ev) =>
      this.handle_connection_add(ev as Connection)
    );
    this.editor.events.beforeRemoveConnection.addListener(token, (ev) =>
      this.handle_connection_remove(ev as Connection)
    );

    this.wsWrapper.on("on_connected", (id) => this.handle_connected(id));
    this.wsWrapper.on("on_disconnected", (code, reason) =>
      this.handle_disconnected(code, reason)
    );

    this.wsWrapper.subscribe(topic_t.add_node, (m, o) =>
      this.handle_server_add_node(m, o)
    );
    this.wsWrapper.subscribe(topic_t.remove_node, (m, o) =>
      this.handle_server_remove_node(m, o)
    );
    this.wsWrapper.subscribe(topic_t.add_connection, (m, o) =>
      this.handle_server_add_connection(m, o)
    );
    this.wsWrapper.subscribe(topic_t.remove_connection, (m, o) =>
      this.handle_server_remove_connection(m, o)
    );
  }

  handle_connected(id: number) {
    this.connected = true;

    this.wsWrapper.send(
      { action: action_t.command, topic: topic_t.get_config },
      (msg) => {
        console.log(msg);
      }
    );
  }

  handle_disconnected(code: number, reason: string) {
    this.connected = false;

    this.editor.connections.forEach((con) => {
      this.connections_to_be_removed.add(con);
      this.editor.removeConnection(con);
    });

    this.editor.nodes.forEach((node) => {
      this.nodes_to_be_removed.add(node);
      this.editor.removeNode(node);
    });
  }

  handle_server_add_node(msg: message, is_origin: boolean) {
    //
  }

  handle_server_remove_node(msg: message, is_origin: boolean) {
    //
  }

  handle_server_add_connection(msg: message, is_origin: boolean) {
    //
  }

  handle_server_remove_connection(msg: message, is_origin: boolean) {
    //
  }

  handle_server_get_config(msg: message) {
    //
  }

  handle_node_add(node: Node): boolean {
    setTimeout(() => {
      console.log((node as any).position.x);

      this.wsWrapper.send(
        { action: action_t.command, topic: "create_node" },
        (msg) => {
          if (msg.action !== action_t.error) {
            this.nodes_to_be_removed.add(node);
            this.editor.removeNode(node);
          }
        }
      );
    }, 0);

    return false;
  }

  handle_node_remove(node: Node): boolean {
    if (this.nodes_to_be_removed.delete(node)) {
      this.positions.remove(node.id);
      return true;
    }

    this.wsWrapper.send(
      { action: action_t.command, topic: topic_t.remove_node },
      (msg) => {
        if (msg.action == action_t.error) {
          this.nodes_to_be_removed.add(node);
          this.editor.removeNode(node);
        }
      }
    );

    return false;
  }

  handle_connection_add(con: Connection): boolean {
    this.wsWrapper.send(
      {
        action: action_t.command,
        topic: topic_t.add_connection,
        from: con.from.id,
        to: con.to.id,
      },
      (msg) => {
        if (msg.action === action_t.error) {
          this.connections_to_be_removed.add(con);
          this.editor.removeConnection(con);
        }
      }
    );

    return false;
  }

  handle_connection_remove(con: Connection): boolean {
    if (this.connections_to_be_removed.delete(con)) {
      return true;
    }

    this.wsWrapper.send(
      {
        action: action_t.command,
        topic: topic_t.remove_connection,
        from: con.from.id,
        to: con.to.id,
      },
      (msg) => {
        if (msg.action !== action_t.error) {
          this.connections_to_be_removed.add(con);
          this.editor.removeConnection(con);
        }
      }
    );

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
