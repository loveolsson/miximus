import { Editor, NodeInterface, Node, Connection } from "@baklavajs/core";

export class helpers {
  static get_interface_name(iface: NodeInterface): string | undefined {
    const parent = iface.parent;
    for (const [name, _iface] of parent.interfaces) {
      if (_iface === iface) {
        return name;
      }
    }

    return undefined;
  }

  static find_node(editor: Editor, id: string): Node | undefined {
    for (const node of editor.nodes) {
      if (node.id === id) {
        return node;
      }
    }

    return undefined;
  }

  static find_connection(
    connections: Set<Connection>,
    from: NodeInterface,
    to: NodeInterface
  ): Connection | undefined {
    for (const c of connections) {
      if (c.from === from && c.to === to) {
        return c;
      }
    }

    return undefined;
  }

  static find_interface(
    editor: Editor,
    id: string,
    name: string
  ): NodeInterface | undefined {
    const node = helpers.find_node(editor, id);
    if (!node) {
      return undefined;
    }

    try {
      return node.getInterface(name);
    } catch (e) {
      return undefined;
    }
  }
}
