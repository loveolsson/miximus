import EventEmitter from "eventemitter3";
import {
  action_e,
  command_s,
  error_s,
  message_s,
  socket_info_s,
  subscribe_s,
  topic_e,
} from "./messages";

interface ws_events {
  on_connected: [number];
  on_disconnected: [number, string];
}

export type message_callback_t<T extends message_s> = (
  msg: T | error_s,
  is_origin: boolean
) => void;

export class ws_wrapper extends EventEmitter<ws_events> {
  private ws?: WebSocket;
  private info?: socket_info_s;
  private ping_timer?: number;
  private callbacks = new Map<string, message_callback_t<message_s>>();
  private subscriptions = new Map<
    topic_e,
    Set<message_callback_t<message_s>>
  >();
  private next_token = 0;
  private closing = false;

  constructor() {
    super();
    this.connect();
  }

  private connect(): void {
    if (this.closing) {
      return;
    }

    this.ws = new WebSocket(`ws://${location.hostname}:7351/`);
    this.ws.onopen = this.handle_open.bind(this);
    this.ws.onclose = this.handle_close.bind(this);
    this.ws.onmessage = this.handle_message.bind(this);
    this.ws.onerror = this.handle_error.bind(this);
  }

  private handle_open(ev: Event): void {
    //
  }

  private handle_close(ev: CloseEvent): void {
    clearInterval(this.ping_timer);
    this.callbacks.clear();

    if (this.info) {
      this.info = undefined;
      this.emit("on_disconnected", ev.code, ev.reason);
    }

    if (this.ws) {
      this.ws.close();
      this.ws = undefined;
    }

    if (!this.closing) {
      setTimeout(this.connect.bind(this), 2000);
    }
  }

  private handle_message(msg: MessageEvent<string>): void {
    if (typeof msg.data !== "string") {
      console.error("Received something other than string on WebSocket");
    }

    try {
      const payload: message_s = JSON.parse(msg.data);

      if (!payload || !payload.action) {
        return;
      }

      if (payload.action !== action_e.ping) {
        console.log("Received", payload);
      }

      switch (payload.action) {
        case action_e.ping:
          return this.handle_ping();
        case action_e.socket_info:
          return this.handle_socket_info(payload as socket_info_s);
        case action_e.result:
        case action_e.error:
          return this.handle_result(payload);
        case action_e.command:
          return this.handle_command(payload as command_s);
        default:
          console.error("Unhandled message");
      }
    } catch (e) {
      console.error(e);
    }
  }

  private handle_socket_info(info: socket_info_s): void {
    console.log(`Received socket info with id ${info.id}`);
    this.info = info;

    for (const topic of this.subscriptions.keys()) {
      const payload: subscribe_s = {
        action: action_e.subscribe,
        topic: topic,
      };

      this.send(payload, (result) => {
        if (result.action === action_e.error) {
          console.info(
            `Failed to re-subscribe to ${topic} with: ${
              (result as error_s).error
            }`
          );
        }
      });
    }

    this.emit("on_connected", info.id);
    this.send_ping();
  }

  private handle_error(): void {
    //console.error(`WebSocket error: ${ev.message}`);
  }

  private handle_command(msg: command_s): void {
    const topic = this.subscriptions.get(msg.topic);
    if (!topic) {
      return console.warn(
        `Receiving command for topic ${msg.topic} not currently subscribed to`
      );
    }

    for (const cb of topic) {
      const is_origin =
        this.info !== undefined && this.info.id === msg.origin_id;
      cb(msg, is_origin);
    }
  }

  private handle_result(msg: message_s): void {
    if (!msg.token) {
      return;
    }

    const cb = this.callbacks.get(msg.token);
    if (!cb) {
      return console.warn("Unknown token in response");
    }

    cb(msg, true);
    this.callbacks.delete(msg.token);
  }

  private send_ping(): void {
    if (this.info) {
      this.send({ action: action_e.ping });
      this.ping_timer = setTimeout(
        this.handle_no_pong_response.bind(this),
        2000
      );
    }
  }

  private handle_ping(): void {
    clearTimeout(this.ping_timer);
    this.ping_timer = setTimeout(this.send_ping.bind(this), 5000);
  }

  private handle_no_pong_response(): void {
    console.error("Server failed to respond to ping, closing WebSocket");
    this.ws?.close();
  }

  public send<T extends message_s, R extends message_s = message_s>(
    msg: T,
    cb?: message_callback_t<R>
  ): boolean {
    if (!this.info) {
      return false;
    }

    if (cb) {
      const token = String(this.next_token++);
      this.callbacks.set(token, cb as message_callback_t<message_s>);
      msg.token = token;
    }

    if (msg.action !== action_e.ping) {
      console.log("Sent", msg);
    }

    this.ws?.send(JSON.stringify(msg));
    return true;
  }

  public subscribe<T extends message_s>(
    topic: topic_e,
    cb: message_callback_t<T>
  ) {
    let sub = this.subscriptions.get(topic);
    if (!sub) {
      sub = new Set();
      this.subscriptions.set(topic, sub);
    }

    if (sub.has(cb as message_callback_t<message_s>)) {
      return console.warn(
        `Subscribing to ${topic} with callback already in set`
      );
    }

    sub.add(cb as message_callback_t<message_s>);

    if (sub.size === 1) {
      this.send({ action: action_e.subscribe, topic }, (msg) => {
        console.info(`Subscribe to ${topic} with: ${msg.action}`);
      });
    }
  }

  public unsubscribe<T extends message_s>(
    topic: topic_e,
    cb: message_callback_t<T>
  ): void {
    const sub = this.subscriptions.get(topic);
    if (!sub) {
      return console.warn(
        `Unsubscribing to topic ${topic} not currently subscribed to`
      );
    }

    if (!sub.delete(cb as message_callback_t<message_s>)) {
      return console.warn(
        `Unsubscribing with callback not registered to ${topic}`
      );
    }

    if (sub.size === 0) {
      this.send({ action: action_e.unsubscribe, topic }, (msg) => {
        console.info(`Unsubscribe to ${topic} with: ${msg.action}`);
      });
    }
  }

  public destroy() {
    this.closing = true;
    this.ws?.close();
  }
}
