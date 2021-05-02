import EventEmitter from "eventemitter3";

export enum action_t {
  subscribe = "subscribe",
  unsubscribe = "unsubscribe",
  ping = "ping",
  socket_info = "socket_info",
  command = "command",
  result = "result",
  error = "error",
}

export enum topic_t {
  add_node = "add_node",
  remove_node = "remove_node",
  add_connection = "add_connection",
  remove_connection = "remove_connection",
  position_node = "position_node",
  get_config = "get_config",
}

export interface message {
  action: action_t;
  token?: string;
}

export interface socket_info extends message {
  id: number;
}

export interface subscribe extends message {
  topic: topic_t;
}

export interface unsubscribe extends message {
  topic: topic_t;
}

export interface command extends message {
  topic: topic_t;
  origin_id?: number;
}

export interface result extends message {
  topic: topic_t;
}

export interface error extends message {
  topic: topic_t;
}

interface ws_events {
  on_connected: [number];
  on_disconnected: [number, string];
}

export type message_callback_t = (msg: message, is_origin: boolean) => void;

export class ws_wrapper extends EventEmitter<ws_events> {
  private ws?: WebSocket;
  private info?: socket_info;
  private ping_timer?: number;
  private callbacks = new Map<string, message_callback_t>();
  private subscriptions = new Map<topic_t, Set<message_callback_t>>();
  private next_token = 0;

  constructor() {
    super();
    this.connect();
  }

  private connect(): void {
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
      console.log("Closed", ev.code, ev.reason);
      this.info = undefined;
      this.emit("on_disconnected", ev.code, ev.reason);
    }

    if (this.ws) {
      this.ws.close();
      this.ws = undefined;  
    }


    setTimeout(this.connect.bind(this), 2000);
  }

  private handle_message(msg: MessageEvent<string>): void {
    if (typeof msg.data !== "string") {
      console.error("Received something other than string on WebSocket");
    }

    try {
      const payload: message = JSON.parse(msg.data);

      if (!payload || !payload.action) {
        return;
      }

      switch (payload.action) {
        case action_t.ping:
          return this.handle_ping();
        case action_t.socket_info:
          return this.handle_socket_info(payload as socket_info);
        case action_t.result:
        case action_t.error:
          return this.handle_result(payload);
        case action_t.command:
          return this.handle_command(payload as command);
        default:
          console.error("Unhandled message");
      }
    } catch (e) {
      console.error(e);
    }
  }

  private handle_socket_info(info: socket_info): void {
    console.log(`Received socket info with id ${info.id}`);
    this.info = info;

    for (const topic of this.subscriptions.keys()) {
      console.log(`Re-subscribing to ${topic}`);
      this.send({ action: action_t.subscribe, topic }, (msg) => {
        console.info(`Re-subscribe to ${topic} with: ${msg.action}`);
      });
    }

    this.emit("on_connected", info.id);
    this.send_ping();
  }

  private handle_error(ev: Event): void {
    //console.error(`WebSocket error!`);
  }

  private handle_command(msg: command): void {
    const topic = this.subscriptions.get(msg.topic);
    if (!topic) {
      return console.warn(
        `Receiving command for topic ${msg.topic} not currently subscribed to`
      );
    }

    for (const cb of topic) {
      const is_origin =
        this.info !== undefined && this.info.id == msg.origin_id;
      cb(msg, is_origin);
    }
  }

  private handle_result(msg: message): void {
    if (!msg.token) {
      return console.warn("No token in response");
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
      this.send({ action: action_t.ping });
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

  public send<T extends message>(msg: T, cb?: message_callback_t): boolean {
    if (!this.info) {
      return false;
    }

    if (cb) {
      const token = String(this.next_token++);
      this.callbacks.set(token, cb);
      msg.token = token;
    }

    this.ws?.send(JSON.stringify(msg));
    return true;
  }

  public subscribe(topic: topic_t, cb: message_callback_t) {
    let sub = this.subscriptions.get(topic);
    if (!sub) {
      sub = new Set();
      this.subscriptions.set(topic, sub);
    }

    if (sub.has(cb)) {
      return console.warn(
        `Subscribing to ${topic} with callback already in set`
      );
    }

    sub.add(cb);

    if (sub.size == 1) {
      this.send({ action: action_t.subscribe, topic }, (msg) => {
        console.info(`Subscribe to ${topic} with: ${msg.action}`);
      });
    }
  }

  public unsubscribe(topic: topic_t, cb: message_callback_t): void {
    const sub = this.subscriptions.get(topic);
    if (!sub) {
      return console.warn(
        `Unsubscribing to topic ${topic} not currently subscribed to`
      );
    }

    if (!sub.delete(cb)) {
      return console.warn(
        `Unsubscribing with callback not registered to ${topic}`
      );
    }

    if (sub.size == 0) {
      this.send({ action: action_t.unsubscribe, topic }, (msg) => {
        console.info(`Unsubscribe to ${topic} with: ${msg.action}`);
      });
    }
  }
}
