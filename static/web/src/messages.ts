export const enum action_t {
  subscribe = "subscribe",
  unsubscribe = "unsubscribe",
  ping = "ping",
  socket_info = "socket_info",
  command = "command",
  result = "result",
  error = "error",
}

export const enum topic_t {
  add_node = "add_node",
  remove_node = "remove_node",
  add_connection = "add_connection",
  remove_connection = "remove_connection",
  update_node = "update_node",
  config = "config",
}

export type position_t = [number, number];

export interface options_s {
  position?: position_t;
  name?: string;
  [index: string]: any;
}

export interface node_s {
  id: string;
  type: string;
  options?: options_s;
}

export interface connection_s {
  from_node: string;
  from_interface: string;
  to_node: string;
  to_interface: string;
}

export interface message_s {
  action: action_t;
  token?: string;
}

export interface socket_info_s extends message_s {
  action: action_t.command;
  id: number;
}

export interface subscribe_s extends message_s {
  action: action_t.subscribe;
  topic: topic_t;
}

export interface unsubscribe_s extends message_s {
  action: action_t.unsubscribe;
  topic: topic_t;
}

export interface command_s extends message_s {
  action: action_t.command;
  topic: topic_t;
  origin_id?: number;
}

export interface command_add_node_s extends command_s {
  topic: topic_t.add_node;
  node: node_s;
}

export interface command_update_node_s extends command_s {
  topic: topic_t.update_node;
  id: string;
  options?: options_s;
}

export interface command_remove_node_s extends command_s {
  topic: topic_t.remove_node;
  id: string;
}

export interface command_add_connection_s extends command_s {
  topic: topic_t.add_connection;
  connection: connection_s;
}

export interface command_remove_connection_s extends command_s {
  topic: topic_t.remove_connection;
  connection: connection_s;
}

export interface command_config_s extends command_s {
  topic: topic_t.config;
}

export interface result_s extends message_s {
  action: action_t.result;
}

interface config_s {
  nodes: node_s[];
  connections: connection_s[];
}

export interface result_config_s extends result_s {
  config: config_s;
}

export interface error_s extends message_s {
  action: action_t.error;
  error: string;
}
