# Miximus architecture

## System overview

Miximus is a native C++20 real-time video application driven by a persisted node graph. Nodes represent sources, generators, GPU transformations, and outputs. The native process owns graph state and validates all mutations. A Vue 3/Baklava client edits a synchronized projection of that graph over WebSocket.

The main loop in `src/main.cpp` targets 60 Hz. It loads settings, starts the embedded web server, runs `core::node_manager_s::tick_one_frame()`, polls GLFW, advances flick timestamps, sleeps to the next frame deadline, and saves the graph during ordered shutdown.

## Runtime ownership and threads

`core::app_state_s` owns application-wide services:

- the root hidden OpenGL context;
- the configuration `boost::asio::io_context`, work guard, and configuration thread;
- a small FiberPool used for explicitly submitted background work;
- DeckLink, NDI, and font registries;
- the thread-safe node-status registry.

The main thread is the render thread. Normal node `prepare`, `execute`, and `complete` calls happen there. The project is not a generally parallel graph executor. Multithreading is explicit:

- WebSocket/configuration callbacks mutate the authoritative graph on the configuration thread.
- DeckLink and SDK callbacks run on SDK-owned threads.
- NDI discovery and output use dedicated threads.
- Teleprompter/font work may be submitted to the application pool.

Each explicit background path owns its locks, queues, completion signals, and shutdown procedure.

## Authoritative graph and render snapshot

`core::node_manager_s` owns two node maps:

- `nodes_`: authoritative configuration, protected by `nodes_mutex_` and mutated by configuration commands;
- `nodes_copy_`: stable render-thread snapshot used for one frame.

Configuration changes mark node IDs dirty or removed. At frame start, the render thread copies only those records into `nodes_copy_`. This provides three important properties:

1. configuration does not remain locked during rendering;
2. every frame sees stable node state;
3. nodes copied to the render graph are destroyed on the render thread, where GL cleanup is safe.

Node records contain a shared `node_i` instance plus copyable `node_state_s`. State contains sanitized JSON options and per-interface connection sets.

## Frame lifecycle

The order in `node_manager_s::tick_one_frame()` is an invariant:

1. Make the root GL context current.
2. Apply dirty and removed records to `nodes_copy_`.
3. Clear `frame_info.executed_nodes`.
4. Call `prepare()` on every render-snapshot node and collect nodes setting `traits.must_run`.
5. Execute those roots. Resolving an input recursively executes its upstream node.
6. Record executed IDs so each node executes at most once per frame.
7. Call `gpu::context_s::finish()` after all submitted `execute()` work.
8. Call `complete()` on every node.
9. Rewind the root GL context.
10. Flush and broadcast node-status deltas.

### Node lifecycle responsibilities

- `init()`: lightweight one-time setup after construction; there is no GL context.
- `prepare()`: read options, update status, create lazy render resources, and set `must_run` for roots/outputs.
- `execute()`: resolve inputs and submit render work with the root GL context current.
- `complete()`: consume completed readbacks or enqueue data after the global `glFinish`; avoid slow I/O.
- destructor: GL-resource destruction is arranged on the render thread with a context current.

## Nodes, interfaces, and connections

Native nodes derive from `nodes::node_i`. Interfaces are members registered in the node constructor with `register_interface()`.

Supported native interface types are:

- `double` (`f64`)
- `gpu::vec2_t`
- `gpu::rect_s`
- `gpu::texture_s*`
- `gpu::framebuffer_s*`

`input_interface_s<T>::resolve_value()` follows its connection and lazily executes the upstream node before reading its output value. `resolve_values()` is used only after increasing the interface's connection limit.

Compatibility and native implicit conversions live in `src/nodes/interface.cpp`. Matching web types and conversions live in `web/src/nodes/interface_types.ts`; keep them consistent.

Framebuffer outputs intentionally allow one downstream connection because they represent ordered mutation. Texture outputs may fan out. Use the framebuffer-to-texture utility node before fan-out.

The node manager validates graph mutations. It:

- rejects missing nodes/interfaces and incompatible types/directions;
- normalizes a connection declared input-to-output into output-to-input;
- rejects cycles;
- enforces maximum connection counts and removes displaced connections;
- marks affected node records dirty;
- broadcasts only accepted authoritative mutations.

## Options and persistence

Each node supplies `get_default_options()` and validates updates in `test_option()`. `node_i::set_options()` starts from defaults, accepts common options or node-specific validated keys, and stores sanitized values. Common options include the display name and editor position.

The persisted settings JSON contains node IDs/types/options and connections. Runtime status is returned with config responses for client initialization but is not persisted as node options.

`main.cpp` loads settings before installing adapters so the initial import does not broadcast mutation events. On shutdown it removes adapters, saves the authoritative graph, and clears nodes with GL current.

## WebSocket and web-client synchronization

The embedded server and protocol are implemented in:

- `src/web_server/`: HTTP/WebSocket transport and static-file serving;
- `src/core/adapters/adapter_websocket.cpp`: graph command translation and authoritative broadcasts;
- `src/types/action.hpp` and `src/types/topic.hpp`: native protocol enums;
- `web/src/messages.ts`: TypeScript payload and enum mirror;
- `web/src/App.vue`: subscriptions and initial config restore;
- `web/src/server_sync.ts`: Baklava-to-server synchronization and feedback-loop suppression.

Commands use request tokens for result/error correlation. Broadcast mutations include `origin_id`; clients normally ignore their own echo. Connection removal is deliberately processed even for the originating client because the server may remove an older connection as a side effect of enforcing connection limits.

The browser graph is never authoritative. Update native validation/defaults first, then mirror the accepted shape in TypeScript.

## Runtime status and registry versions

Nodes publish runtime information with:

```cpp
status_registry->write(node_id, key, value);
```

Writes are thread-safe and ignored when the stored value is unchanged. At frame end, pending keys are merged into one delta object per node. WebSocket broadcasts contain only those deltas, and `web/src/nodes/status_store.ts` shallow-merges them. Initial config and explicit `node_status` requests return full snapshots.

Long lists must not be rebuilt every frame. DeckLink, NDI, and font registries expose atomic version counters. Nodes remember the last version and regenerate lists only after a change. Dependent lists also track their selection; for example, changing a font family regenerates `font_variants` without rebuilding `font_names`.

Use `StatusDropdownInterface` for a web option populated from a status list. Its list key must exactly match the native status key.

## Key implementation files

- `src/main.cpp`
- `src/core/app_state.hpp/.cpp`
- `src/core/node_manager.hpp/.cpp`
- `src/core/node_status_registry.hpp/.cpp`
- `src/nodes/node.hpp/.cpp`
- `src/nodes/interface.hpp/.cpp`
- `src/nodes/node_map.hpp`
- `src/core/adapters/adapter_websocket.hpp/.cpp`
- `web/src/App.vue`
- `web/src/server_sync.ts`
- `web/src/messages.ts`
