# Miximus architecture

## System overview

Miximus is a native C++20 real-time video application driven by a persisted node graph. Nodes represent sources, generators, GPU transformations, and outputs. The native process owns graph state and validates all mutations. A Vue 3/Baklava client edits a synchronized projection of that graph over WebSocket.

The main loop defaults to a 60 Hz cadence. It loads settings, starts the embedded web server, runs
`core::node_manager_s::tick_one_frame()`, polls GLFW, and lets `core::frame_scheduler_s` pace the next evaluation from
a stable clock anchor. The configured rational frame rate is read once from the reserved `$app` node's render-snapshot
state at the start of each evaluation and stored in immutable frame-local settings on `app_state_s`. The scheduler
produces the matching `frame_context_s`. Nodes and workers read those frame-local values and track changes rather than
assuming the default. `app_state_s::frame_info` contains only the submission and execution traversal state owned by the
current frame.

The scheduler currently uses an internal `steady_clock` source and a provisional policy that permits the next
evaluation to begin up to one frame late. Older evaluations are skipped in one decision: frame number and PTS advance
together while the clock anchor remains fixed. Frame-rate changes start a new epoch and re-anchor the mapping. The
scheduler publishes rate-limited timing, deadline, skip, and overload metrics through the reserved `$app` status.

The planned transition to a configurable program timeline, source clock recovery, PTS-aware media selection, buffered
outputs, and frame-atomic graph transactions is documented in
[frame-timing-and-synchronization.md](frame-timing-and-synchronization.md). That document describes future behavior;
this document continues to describe the current runtime until each migration stage is implemented.

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
2. Apply dirty and removed records to `nodes_copy_` and read the reserved settings node from that stable snapshot.
3. Create the immutable frame context for this evaluation.
4. Call `prepare()` on every render-snapshot node and collect the sinks that demand a frame.
5. Recursively call `submit()` from every demanding sink. Each node follows the input connections it may need through
   `interface_i`; a dedicated visited set ensures that shared upstream nodes submit only once.
6. Finish the complete submission traversal before execution begins.
7. Execute the demanding sinks. Resolving an input recursively executes its upstream node.
8. Record executed IDs so each node executes at most once per frame.
9. Call `gpu::context_s::finish()` after all submitted `execute()` work.
10. Call `complete()` on every node.
11. Rewind the root GL context.
12. Flush and broadcast node-status deltas.
13. Poll GLFW, measure completion, skip obsolete evaluations if necessary, and wait for the next anchored target.

### Node lifecycle responsibilities

- `init()`: lightweight one-time setup after construction; there is no GL context.
- `prepare()`: advance all-node state, read options, update status, create lazy render resources, and report whether a
  sink demands execution.
- `submit()`: park frame-local work or initiate asynchronous work for the demanded upstream closure without waiting.
- `execute()`: resolve inputs and submit render work with the root GL context current.
- `complete()`: consume completed readbacks or enqueue data after the global `glFinish`; avoid slow I/O.
- destructor: GL-resource destruction is arranged on the render thread with a context current.

Submission is conservative for routing controlled by a connected interface: a submitted node is not guaranteed to
execute in that evaluation. Work started by `submit()` must therefore remain owned by a bounded service or queue until
it is consumed, superseded, cancelled, or shut down. `complete()` must not assume that submission implies execution.

## Nodes, interfaces, and connections

Native nodes derive from `nodes::node_i`. Interfaces are members constructed with the owning node and register
themselves automatically. Duplicate interface names fail node construction.

Supported native interface types are:

- `double` (`f64`)
- `gpu::vec2_t`
- `gpu::rect_s`
- `gpu::texture_s*`
- `gpu::framebuffer_s*`

`input_interface_s<T>::resolve_value()` follows its connection and lazily executes the upstream node before reading its output value. `resolve_values()` is used only after increasing the interface's connection limit.

Connection traversal belongs to `interface_i` in both passes. During submission, a node asks the relevant input
interfaces to submit their connected producers recursively; during execution, typed input resolution executes connected
producers recursively before reading their values. The two passes use separate visited sets. The default `node_i::submit`
visits every input interface. Routing nodes override it to narrow the traversal when an option already determines the
route, or conservatively visit every possible branch when the selector is connected and cannot be resolved without
execution.

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

Each node supplies `get_default_options()` and normalizes updates in `normalize_option()`. Normalization reports whether
the value was accepted unchanged, corrected, or invalid. `node_i::set_options()` applies a complete update atomically,
rejects invalid keys or values, and stores accepted normalized values. Update broadcasts report whether any values were
corrected. Common options include the display name and editor position.

The persisted settings JSON is owned by `core::configuration_s`. Its JSON load/serialization API is separate from the
file wrappers so future web commands can reuse the same path. The document contains a top-level `schema_version`, node
IDs, types, per-node schema versions, options, and connections. Application settings are an ordinary `node_i` with the
reserved `$app` ID and are persisted in that same node array. The node has no interfaces or render work; external
creation under another ID, removal, and connections are rejected. The bundled client omits it from the visible
Baklava graph, while updates and runtime status continue to use the normal node channels. Runtime status is added only
to client snapshots and is not written to disk.

Unversioned documents and nodes are the version 1 baseline. When a node schema changes, its registered transitions migrate the options JSON in place. Connections replay the output-interface migrations for their source node and the input-interface migrations for their destination node over the same version range. Migration completes before the existing node and connection construction paths are called; any missing transition or construction error aborts startup.

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

Writes are thread-safe and ignored when the stored value is unchanged. Nodes publishing several related values use a
scoped per-node writer so the registry is locked and the node ID is looked up only once. Changed values are accumulated
directly into one pending delta object per node, which is moved into the WebSocket broadcast at frame end;
`web/src/nodes/status_store.ts` shallow-merges it. Initial config and explicit `node_status` requests return full
snapshots.

Long lists must not be rebuilt every frame. DeckLink, NDI, and font registries expose atomic version counters. Nodes remember the last version and regenerate lists only after a change. Dependent lists also track their selection; for example, changing a font family regenerates `font_variants` without rebuilding `font_names`.

Use `StatusDropdownInterface` for a web option populated from a status list. Its list key must exactly match the native
status key. Lists contain ordered `{id, label}` objects: persisted options and client updates use the stable `id`, while
the dropdown presents the human-readable `label`.

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
