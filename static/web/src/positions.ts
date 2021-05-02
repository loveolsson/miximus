import { ViewPlugin } from "@baklavajs/plugin-renderer-vue";
import { action_t, topic_t, ws_wrapper } from "./websocket";
import { throttle, DebouncedFunc } from "lodash";

interface NodeInfo {
  pos: [number, number];
  waiting: number;
  debounce: DebouncedFunc<(pos: [number, number]) => void>;
}

export class Positions {
  infos = new Map<string, NodeInfo>();

  constructor(private view: ViewPlugin, private ws: ws_wrapper) {
    const token = {};
    view.hooks.renderNode.tap(token, (view) => {
      this.handle_move(view.data.id, [
        view.data.position.x,
        view.data.position.y,
      ]);

      return view;
    });
  }

  handle_move(id: string, pos: [number, number]) {
    let info = this.infos.get(id);
    if (!info) {
      info = {
        pos,
        waiting: 0,
        debounce: throttle(
          (pos) => {
            this.send_position(id, pos);
          },
          500,
          { leading: true, trailing: true }
        ),
      };
      this.infos.set(id, info);
    }

    info.debounce(pos);
  }

  send_position(id: string, pos: [number, number]) {
    const info = this.infos.get(id);
    if (!info) {
      return;
    }

    this.ws.send(
      { action: action_t.command, topic: topic_t.position_node, id, pos },
      () => {
        info.waiting--;
      }
    );
  }

  handle_response(id: string) {
    //
  }

  set_position(id: string, x: number, y: number): void {
    const node = this.view.editor.nodes.find((node) => node.id === id);
    if (!node) {
      return;
    }
  }

  remove(id: string) {
    const info = this.infos.get(id);
    if (info) {
      info.debounce.cancel();
    }
    this.infos.delete(id);
  }
}
