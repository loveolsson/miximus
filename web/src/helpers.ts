import type { Graph, AbstractNode, NodeInterface } from "@baklavajs/core";

export function find_node(graph: Graph, id: string): AbstractNode | undefined {
  return graph.nodes.find((n) => n.id === id);
}

export function find_interface(
  graph: Graph,
  nodeId: string,
  name: string,
): NodeInterface | undefined {
  const node = find_node(graph, nodeId);
  if (!node) return undefined;
  return node.inputs[name] ?? node.outputs[name];
}

export function find_node_and_key_for_interface(
  graph: Graph,
  intf: NodeInterface,
): [AbstractNode, string] | undefined {
  for (const node of graph.nodes) {
    for (const [key, i] of Object.entries(node.inputs)) {
      if (i === intf) return [node, key];
    }
    for (const [key, i] of Object.entries(node.outputs)) {
      if (i === intf) return [node, key];
    }
  }
  return undefined;
}

export function find_connection(graph: Graph, fromIntf: NodeInterface, toIntf: NodeInterface) {
  return graph.connections.find((c) => c.from === fromIntf && c.to === toIntf);
}
