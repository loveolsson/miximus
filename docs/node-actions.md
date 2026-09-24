# Node actions

Node actions are transient commands for individual nodes. They use the existing WebSocket request/reply channel;
they are not options, graph mutations, broadcasts, or persisted configuration. Adding an action requires a node
handler and a caller, without changing the shared protocol or adding an action to a global enum.

## Wire contract

```json
{
  "action": "command",
  "topic": "node_action",
  "token": "request-42",
  "id": "browser-node-id",
  "name": "reload",
  "payload": { "ignore_cache": false }
}
```

All four request fields (`token`, `id`, `name`, `payload`) are required. Payload may be any JSON value; use `{}` for
an action without arguments. Tokens and node IDs are limited to 256 bytes, names to 128 bytes, and serialized
payloads to 64 KiB. Payloads/results also have a maximum nesting depth of 64 and 16,384 JSON values, checked before
copying or serialization. Envelope/limit violations return `malformed_payload` at the wire boundary; native queue
callers receive `invalid_payload`. IDs, names and tokens must be nonempty. The target node validates supported names
and payload schemas.

A successful handler returns an arbitrary JSON result:

```json
{ "action": "result", "token": "request-42", "data": null }
```

Errors use the existing `error_s` envelope with the same token and an optional explanatory message. Relevant codes
are `malformed_payload`, `invalid_payload`, `not_found`, `unsupported_action`, `unavailable`, `busy`, `expired`,
`cancelled`, and `internal_error`. Results are limited to 64 KiB and explanatory messages to 1 KiB.

Success acknowledges that the handler ran and accepted/performed the action. If it scheduled background work,
success does **not** mean that work finished. Publish its progress and eventual failures through normal node status.

## Delivery and ownership

The manager validates the target against the authoritative graph and captures a weak reference to that exact node
instance. JSON validation and copying happen outside the graph and inbox locks. The bounded inbox owns the request
values and reply callback, not the node. Removing a node and reusing its ID cannot redirect an old request to the
replacement.

At the frame boundary, graph changes and the action batch are selected under the same graph lock. Dispatch happens
on the render thread, after `begin_frame()` and before `prepare()`, outside graph/queue locks. Handlers receive the
stable frame snapshot's options, including changes accepted before that boundary. A removal or option update after
the boundary applies next frame, just like other graph changes. A handler can run before the node's first `prepare()`.

The inbox admits at most 64 queued requests, at most eight per node ID, and takes at most 16 per frame. Requests
expire after five seconds when checked at dispatch. Queue capacity is released when a batch is taken, so at most
64 queued plus 16 selected requests are retained. Excess work is rejected as `busy`; it never creates an unbounded
render-thread backlog. Accepted requests preserve FIFO order.

Each accepted request gets one reply attempt when dispatched, expired, or cancelled. Handler exceptions become
`internal_error` and do not interrupt other actions. Discarding an undispatched frame batch cancels its requests;
shutdown closes admission and cancels queued requests. Reply failures cannot interrupt the remaining batch. The
WebSocket responder weakly references the server and posts replies to its connection thread.

A disconnected client does not cancel an already accepted action. Requests are never automatically replayed.
A lost reply or client timeout leaves the outcome unknown; a repeated command is a new invocation, even if its
payload or token matches an earlier one. This protocol does not provide reconnect-spanning deduplication.

## Adding an action

Include `nodes/action.hpp` and override `node_i::handle_action(app, state, name, payload)` in the native node:

- Validate the name and the entire payload before side effects. Unsupported names return `unsupported_action`;
  invalid arguments return `invalid_payload`. Nodes without an override reject every action.
- Use the supplied snapshot state to decide whether the operation is available. Return `unavailable` or `busy`
  when appropriate; an action must not implicitly enable a disabled node.
- Keep handling bounded and nonblocking. Do not wait for SDK/network/file work or record GPU commands here.
  Schedule background work through the node's existing owner/worker, or latch state for `prepare()`/`execute()`.
- Return `nodes::action_result_s`, optionally with JSON `data`. Preserve ordinary configuration changes through
  `update_node`; do not mutate the snapshot or use actions as a second configuration store.

The reusable `NodeActionInterface` supplies a row of non-port buttons:

```ts
reload: () => new NodeActionInterface("Reload", [
  { label: "Reload ↻", action: "reload" },
  { label: "Force ↻", action: "reload", payload: { ignore_cache: true } },
]),
```

Each button can supply an optional JSON payload. Custom controls can call
`ws.request<node_action_request_s, node_action_result_s>(message, abortSignal)` directly. The helper adds the token,
limits outstanding waits, and cleans up on replies, disconnect, a ten-second timeout, or abort. Aborting cancels
only the local reply wait. The control aborts that wait on unmount and disables its buttons while waiting. Labels remain unchanged;
errors are logged to the console without inline feedback.
Neither path changes an interface value or writes an option.

## Browser reload

The browser accepts `reload` with `{}` or `{ "ignore_cache": boolean }`; other fields/types are rejected.
The action requires an enabled, ready session matching the current URL, size and frame rate. The owning
`session_request_s` posts reload to CEF's UI thread; the frame-consumer session API retains no lifecycle controls.
Only one reload task may be pending for a session. CEF `Reload()` uses normal cache behavior and
`ReloadIgnoreCache()` explicitly bypasses it.

Reload retains the browser and frame pool, cancels old page commands, and uses the existing navigation/capture-epoch
handling. Completed frames remain available during loading. Normal browser status reports loading, new captured
frames and failures. The action does not increment automatic restart counts, change options, or recreate a failed
session. CEF-disabled builds explicitly reject reload as unavailable.

## Global settings actions

Refresh Fonts is available in Global Settings and uses the existing application-wide font registry command.
It is no longer repeated on individual text and teleprompter nodes.

Clear Browser Cache sends `clear_browser_cache` with `{}` to the application settings node (`$app`). It clears
CEF's shared HTTP cache for all browser nodes, without reloading pages or deleting cookies, local storage, or
service-worker storage. It also works with no active browser nodes. CEF-disabled/unavailable runtimes return
`unavailable`; concurrent clearing returns `busy`. The action acknowledges scheduling, and the settings node's
`browser_cache_clearing` status reports whether the asynchronous operation is still pending. SDK work and completion
run on CEF's UI thread, with no render-thread waiting.

## Validation

`core_test` covers payload ownership, dispatch thread/order/snapshot state, target removal/replacement, queue and
batch limits, expiry, shutdown/abandoned-batch cancellation, exception isolation, and typed wire decoding.
`npm test` in `web/` covers correlated replies, server errors, disconnect without replay, timeout, abort, capacity,
and serialization failures using the real WebSocket wrapper with a controlled transport.

Run `node scripts/test_node_actions.mjs` for an isolated WebSocket/CEF integration check. It serves a local page and
checks both reload variants, font refresh, shared cache eviction without reloading, errors, unchanged
options/restart count, removal and shutdown. For a disabled build,
run `node scripts/test_node_actions.mjs build-cef-off/miximus --cef-disabled`. The script refuses to run alongside
an existing application on port 7351. Use the normal Vulkan validation environment for hardware runs.

Initial node-action validation on the development machine:

- CEF-enabled and CEF-disabled native builds passed, with all 146 CTests passing in each build.
- The four-job tidy build passed without warnings, with all 146 CTests passing.
- The web production build and all five WebSocket wrapper tests passed.
- Both WebSocket integration variants passed with Vulkan validation enabled, with no validation errors.
- The CEF subsystem probe verified that reload cancels old page commands and creates a fresh page context;
  its existing pixel, resource-lifetime and subprocess-recovery checks also passed.

The global-settings controls were additionally checked with CEF-enabled and CEF-disabled native builds (147 tests
passing in each), the web production build and five frontend tests. Both WebSocket integration variants passed
under Vulkan validation, including font refresh, cache eviction without navigation and CEF-disabled rejection.
