import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import NodeView from "@baklavajs/plugin-renderer-vue/dist/baklavajs-plugin-renderer-vue/src/components/node/Node.vue";
import { ws_wrapper } from "./websocket";
import {
  action_t,
  command_update_node_s,
  options_s,
  position_t,
  topic_t,
} from "./messages";
import isEqual from "deep-equal";

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
    view_plugin.hooks.renderNode.tap({}, (view) => {
      const info = this.get_or_create_info(view);
      const new_pos = view_intercept.transform_position(view);

      if (
        !info.options.position ||
        !isEqual(info.options.position, new_pos, { strict: true })
      ) {
        this.handle_move(info, new_pos);
      }

      return view;
    });
  }

  static transform_position(view: NodeView): position_t {
    const pos = view.data.position;
    return [pos.x, pos.y];
  }

  get_or_create_info(view: NodeView): NodeInfo {
    const id = view.data.id;
    let info = this.infos.get(id);
    if (info) {
      return info;
    }

    info = {
      view,
      options: {
        position: view_intercept.transform_position(view),
      },
      new_position: false,
      waiting: wait_state_t.no_change,
    };

    // Disgusting hack
    const old_done_rename = view.doneRenaming;
    view.doneRenaming = () => {
      if (info!.options.name !== view.tempName)
        info!.options.name = view.tempName;
      this.handle_value_change(info!, "name", view.tempName);
      old_done_rename.call(view);
    };

    view.data.events.update.addListener({}, (ev) => {
      if (ev.option) {
        const value = ev.option.value;
        if (info!.options[ev.name] !== value) {
          this.handle_value_change(info!, ev.name, value);
        }
      }
    });

    this.infos.set(id, info);
    return info;
  }

  handle_value_change(info: NodeInfo, key: string, value: any) {
    info.options[key] = value;

    const payload: command_update_node_s = {
      action: action_t.command,
      topic: topic_t.update_node,
      id: info.view.data.id,
      options: {},
    };

    payload.options![key] = value;
    this.ws.send(payload);
  }

  handle_move(info: NodeInfo, position: position_t) {
    info.options.position = position;
    info.new_position = true;

    if (info.waiting === wait_state_t.no_change) {
      this.send_position(info.view.data.id, info);
    }
  }

  send_position(id: string, info: NodeInfo) {
    if (!info.new_position) {
      info.waiting = wait_state_t.no_change;
      return;
    }

    const payload: command_update_node_s = {
      action: action_t.command,
      topic: topic_t.update_node,
      id,
      options: {
        position: info.options.position,
      },
    };

    info.new_position = false;
    info.waiting = wait_state_t.waiting;

    this.ws.send(payload, (msg) => {
      info.waiting = wait_state_t.delayed;
      setTimeout(() => {
        this.send_position(id, info);
      }, 250);
    });
  }

  set_position(id: string, position: position_t): void {
    const node = this.infos.get(id);
    if (!node) {
      return;
    }

    if (node.waiting === wait_state_t.waiting || node.new_position) {
      return;
    }

    node.options.position = position;
    node.view.data.position = {
      x: position[0],
      y: position[1],
    };
  }

  remove(id: string) {
    this.infos.delete(id);
  }
}
