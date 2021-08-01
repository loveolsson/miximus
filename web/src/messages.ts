export const enum action_e {
  subscribe = "subscribe",
  unsubscribe = "unsubscribe",
  ping = "ping",
  socket_info = "socket_info",
  command = "command",
  result = "result",
  error = "error",
}

export const enum topic_e {
  add_node = "add_node",
  remove_node = "remove_node",
  add_connection = "add_connection",
  remove_connection = "remove_connection",
  update_node = "update_node",
  config = "config",
}

export const enum type_e {
  math_i64 = "math_i64",
  math_f64 = "math_f64",
  math_vec2 = "math_vec2",
  math_vec2i = "math_vec2i",
  screen_output = "screen_output",
  decklink_input = "decklink_input",
  decklink_output = "decklink_output",
}

export type position_t = [number, number];

export interface options_s {
  position?: position_t;
  name?: string;
  [index: string]: any;
}

export interface node_s {
  id: string;
  type: type_e;
  options: options_s;
}

export interface connection_s {
  from_node: string;
  from_interface: string;
  to_node: string;
  to_interface: string;
}

export interface message_s {
  action: action_e;
  token?: string;
}

export interface socket_info_s extends message_s {
  action: action_e.command;
  id: number;
}

export interface subscribe_s extends message_s {
  action: action_e.subscribe;
  topic: topic_e;
}

export interface unsubscribe_s extends message_s {
  action: action_e.unsubscribe;
  topic: topic_e;
}

export interface command_s extends message_s {
  action: action_e.command;
  topic: topic_e;
  origin_id?: number;
}

export interface command_add_node_s extends command_s {
  topic: topic_e.add_node;
  node: node_s;
}

export interface command_update_node_s extends command_s {
  topic: topic_e.update_node;
  id: string;
  options: options_s;
}

export interface command_remove_node_s extends command_s {
  topic: topic_e.remove_node;
  id: string;
}

export interface command_add_connection_s extends command_s {
  topic: topic_e.add_connection;
  connection: connection_s;
}

export interface command_remove_connection_s extends command_s {
  topic: topic_e.remove_connection;
  connection: connection_s;
}

export interface command_config_s extends command_s {
  topic: topic_e.config;
}

export interface result_s extends message_s {
  action: action_e.result;
}

interface config_s {
  nodes: node_s[];
  connections: connection_s[];
}

export interface result_config_s extends result_s {
  config: config_s;
}

export interface error_s extends message_s {
  action: action_e.error;
  error: string;
}
