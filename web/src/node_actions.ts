import {
  action_e,
  topic_e,
  type node_action_request_s,
  type node_action_result_s,
} from "./messages";
import type { node_action_contracts_s } from "./generated/json_contracts";
import type { ws_wrapper } from "./websocket";

export type NodeActionNodeType = keyof node_action_contracts_s;

type Command<Name, Payload> = { readonly action: Name } & ({} extends Payload
  ? { readonly payload?: Payload }
  : { readonly payload: Payload });

// Distribute over node types to retain action/payload correlation in generic controls.
export type NodeActionCommand<T extends NodeActionNodeType = NodeActionNodeType> =
  T extends NodeActionNodeType
    ? {
        [K in keyof node_action_contracts_s[T]]: node_action_contracts_s[T][K] extends {
          readonly payload: infer P;
        }
          ? Command<K, P>
          : never;
      }[keyof node_action_contracts_s[T]]
    : never;

type NodeActionResult<T extends NodeActionNodeType, C> = T extends NodeActionNodeType
  ? {
      [K in keyof node_action_contracts_s[T]]: C extends { readonly action: K }
        ? node_action_contracts_s[T][K] extends { readonly result: infer R }
          ? R
          : never
        : never;
    }[keyof node_action_contracts_s[T]]
  : never;

export function requestNodeAction<
  T extends NodeActionNodeType,
  C extends NodeActionCommand<NoInfer<T>>,
>(ws: ws_wrapper, _nodeType: T, id: string, command: C, signal?: AbortSignal) {
  // The node type provides compile-time scope. The server resolves the actual node by ID.
  return ws.request<
    node_action_request_s,
    Omit<node_action_result_s, "data"> & { readonly data: NodeActionResult<T, C> }
  >(
    {
      action: action_e.command,
      topic: topic_e.node_action,
      id,
      name: command.action,
      payload: command.payload === undefined ? {} : command.payload,
    },
    signal,
  );
}
