import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import NodeView from "@baklavajs/plugin-renderer-vue/dist/baklavajs-plugin-renderer-vue/src/components/node/Node.vue";

import { ws_wrapper } from "./websocket";
import {
  action_e,
  command_update_node_s,
  options_s,
  position_t,
  topic_e,
} from "./messages";
import isEqual from "deep-equal";
import { connectionColorMap } from "./nodes/types";

const enum wait_state_t {
  no_change,
  waiting,
  delayed,
}

interface NodeInfo {
  view: NodeView;
  options: options_s;
  new_position: boolean;
  waiting: wait_state_t;
}

export class view_intercept {
  infos = new Map<string, NodeInfo>();

  constructor(view_plugin: ViewPlugin, private ws: ws_wrapper) {
    view_plugin.hooks.renderNode.tap(this, (view) => {
      const info = this.get_or_create_info(view);
      const new_pos = view_intercept.transform_position(view);

      if (
        !info.options.node_visual_position ||
        !isEqual(info.options.node_visual_position, new_pos, { strict: true })
      ) {
        this.handle_move(info, new_pos);
      }

      return view;
    });

    view_plugin.hooks.renderConnection.tap(this, (con) => {
      const color = connectionColorMap.get(con.connection.from.type);
      if (color !== undefined) {
        con.$el.setAttribute("style", "stroke: " + color);
      }

      return con;
    });
  }

  static transform_position(view: NodeView): position_t {
    const pos = view.data.position;
    return [pos.x, pos.y];
  }

  get_or_create_info(view: NodeView): NodeInfo {
    const id = view.data.id;
    const info = this.infos.get(id);
    if (info) {
      return info;
    }

    const newInfo: NodeInfo = {
      view,
      options: {
        node_visual_position: view_intercept.transform_position(view),
      },
      new_position: false,
      waiting: wait_state_t.no_change,
    };

    // Disgusting hack
    const old_done_rename = view.doneRenaming;
    view.doneRenaming = () => {
      if (newInfo.options.name !== view.tempName)
        newInfo.options.name = view.tempName;
      this.handle_value_change(newInfo, "name", view.tempName);
      old_done_rename.call(view);
    };

    view.data.events.update.addListener({}, (ev) => {
      if (ev.option) {
        const value = ev.option.value;
        if (newInfo.options[ev.name] !== value) {
          this.handle_value_change(newInfo, ev.name, value);
        }
      }

      if (ev.interface) {
        const value = ev.interface.value;
        if (!isEqual(newInfo.options[ev.name], value)) {
          this.handle_value_change(newInfo, ev.name, value);
        }
      }
    });

    this.infos.set(id, newInfo);
    return newInfo;
  }

  handle_value_change(info: NodeInfo, key: string, value: unknown): void {
    info.options[key] = value;

    console.log(key, value);

    const payload: command_update_node_s = {
      action: action_e.command,
      topic: topic_e.update_node,
      id: info.view.data.id,
      options: {},
    };

    payload.options[key] = value;
    this.ws.send(payload);
  }

  handle_move(info: NodeInfo, position: position_t): void {
    info.options.node_visual_position = position;
    info.new_position = true;

    if (info.waiting === wait_state_t.no_change) {
      this.send_position(info.view.data.id, info);
    }
  }

  send_position(id: string, info: NodeInfo): void {
    if (!info.new_position) {
      info.waiting = wait_state_t.no_change;
      return;
    }

    const payload: command_update_node_s = {
      action: action_e.command,
      topic: topic_e.update_node,
      id,
      options: {
        node_visual_position: info.options.node_visual_position,
      },
    };

    info.new_position = false;
    info.waiting = wait_state_t.waiting;

    this.ws.send(payload, () => {
      info.waiting = wait_state_t.delayed;
      setTimeout(() => {
        this.send_position(id, info);
      }, 250);
    });
  }

  set_position(id: string, position: position_t): boolean {
    const node = this.infos.get(id);
    if (!node) {
      return false;
    }

    if (node.waiting === wait_state_t.waiting || node.new_position) {
      return true;
    }

    node.options.node_visual_position = position;
    node.view.data.position = {
      x: position[0],
      y: position[1],
    };

    return true;
  }

  remove(id: string): void {
    this.infos.delete(id);
  }
}
