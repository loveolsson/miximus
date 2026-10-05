# Node actions

Node actions are commands interpreted against the authoritative configuration graph. An action may derive settings
changes, perform work independent of rendering, or request work at a frame boundary. The action itself is transient;
its accepted settings changes use the normal update broadcasts and persistence. The WebSocket request/reply contract
is unchanged. Adding an action requires a node handler and a caller, without adding a global protocol enum.

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

All four request fields (`token`, `id`, `name`, `payload`) are required. Token, ID and name must be nonempty strings;
payload may be any JSON value, including `{}` for an action without arguments. Missing/wrongly typed envelope fields
return `malformed_payload`. The target node validates supported names and deserializes the action-specific payload contract.
Known object contracts ignore unknown fields for additive cross-version compatibility; known fields still require
their declared types. The generic envelope continues to preserve arbitrary JSON for other actions.

There are no action-specific byte, nesting, value-count or string-length limits, and no extra serialization pass to
validate parsed JSON. The WebSocket transport caps all received messages at 32,000,000 bytes, including fragmented
messages, before JSON parsing. During parsing, the WebSocket handler also rejects more than 16 nested objects/arrays,
counting the envelope object as level 1. Both limits apply equally to every topic; they are not action validation rules.
Over-deep messages close the connection as invalid JSON before typed decoding or command dispatch.

A successful handler returns an arbitrary JSON result:

```json
{ "action": "result", "token": "request-42", "data": null }
```

Errors use the existing `error_s` envelope with the same token and an optional explanatory message. Relevant codes
are `malformed_payload`, `invalid_payload`, `not_found`, `unsupported_action`, `unavailable`, `busy`, `expired`,
`cancelled`, and `internal_error`. Results and explanatory messages have no action-specific size limit.

A result means whatever completion the action explicitly documents. Existing browser reload and cache clearing
continue to acknowledge scheduling, with later progress in node status. New asynchronous actions may retain an owned
action and reply when their work actually finishes, using the same result/error envelopes.

## One action throughout its lifetime

A well-formed native request is wrapped once in move-only `nodes::action_s`. The same object travels through
configuration admission, pending frame updates, frame dispatch, and optional asynchronous work. It owns its ID,
name, JSON payload, resolved node-instance handle, and response responsibility. Wire token correlation stays in
its responder callback. No settings, borrowed graph references, or transaction policy belong to the action.

`complete(result)` and `fail(error, message)` share one synchronized settlement state with timeout handling.
Only the first settlement attempts a reply. Moving transfers response authority; a moved-from object can inspect
`result_error()` but cannot settle the action. Dropping an owned, unsettled action reports `internal_error`.
Throwing reply callbacks are isolated. Exactly-once settlement means one reply attempt; it cannot guarantee network
delivery after a disconnect.

## Configuration settings and dispatch

Both hooks receive `action_s&` and return `action_dispatch_e`:

```cpp
action_dispatch_e handle_action(action_context_s& context, action_s& action) const final;
action_dispatch_e handle_frame_action(core::app_state_s* app, const node_state_s& state,
                                     action_s& action) final;
```

Keep both hooks as name dispatchers. Dedicated action handlers own payload decoding, validation, and behavior.
The enum is `unhandled`, `handled`, or `frame`. `handled` means replied or transferred responsibility, including
asynchronous work, not necessarily success. The config caller reports generic `unsupported_action` for `unhandled`.
The frame caller reports `internal_error` for any result other than `handled`; the base hooks return `unhandled`.
A handler reporting a normal failure calls `action.fail()` itself and returns `handled`.

Only `handle_action` receives the separate borrowed config capability, `action_context_s`. It exposes the current
settings/connections, read-only connected-node lookup, and `update_settings(patch)`. The manager owns the candidate
settings and normalizes every edit through `set_options`. The handler checks the update result and reports a failure
through its action. A failed action's candidate settings are discarded; successful changes mark the node dirty and
broadcast canonical options. Action-derived broadcasts also reach the originating editor.

Configuration dispatch runs under the graph lock. It must not access render-owned fields or retain the context.
For independent work, a dedicated handler calls `context.defer(handler)` and returns `handled`. The caller moves the
same action into `handler(app, action)` after committing settings and releasing the lock. That handler may finish or
move the whole action into a service worker. It must own all arguments and must not capture the config context or
render-owned node resources. Replies settled during admission are also delivered after unlocking, allowing reentry.

A frame action returns `frame`, leaving ownership with the caller, which moves it into the pending node update.
The dedicated config handler can copy resolved arguments into the action payload when it needs admission-time values.
Otherwise frame handlers see the original payload and the selected frame's final settings. Frame and async handlers
never receive the settings-editing capability. A delayed settings update must submit a new config operation with its
captured target handle, preventing updates to a replacement node with the same ID.

Dedicated handlers may use `action.get_typed_payload<T>()`. It uses nlohmann deserialization and returns an empty
optional for JSON decoding errors, including invalid described enum values. The handler reports `invalid_payload`; dispatch hooks do not decode payloads.
There are no generic action payload-size or complexity limits.

## Native control batches and future transactions

`handle_control_batch()` is an internal batching operation, not a transaction protocol. It validates explicit option
updates atomically, then processes actions individually in order as best effort. Later actions see successful changes
from earlier actions. A failed action does not stop the batch or roll back previously accepted settings.

Every supplied action is wrapped before rejection is possible. If explicit settings validation fails or the manager
is closed, the caller fails every unprocessed action with the batch rejection reason and returns the settings error.
No node action handlers run on that path. Callbacks run outside the graph lock. Individual action errors use their own
responder; the single-action WS adapter must not send another error after a request has been accepted for processing.

Transactions, transaction tokens, and transaction rollback policy are not implemented. A future transaction owner
must answer every deferred or unprocessed action on abort, plus its transaction token; already settled action results
stand. Actions themselves remain unaware of that policy. The WS envelope and client correlation behavior are unchanged.

## Frame delivery, identity, and expiry

Per-node pending update records own whole actions under the same graph lock as settings. At frame start,
`take_frame_updates()` copies the updated node snapshot and moves the actions out exactly once. Dispatch runs before
all-node `prepare()`, regardless of render demand, in admission FIFO order. Actions are never persisted or replayed.

The authoritative graph resolves each action's `node_handle_s`. Removal cancels pending actions with `not_found`;
a replacement gets a fresh identity. Dispatch compares identity with the selected frame snapshot. A removal after
the cutoff does not invalidate the already selected frame. Handles retain identity metadata, never node resources.

There are 64 pending frame actions per node instance, no global or per-frame count cap, and a five-second waiting
expiry. `action_s` owns its deadline and timeout failure. With an application executor, its timer can settle an expired
action even if frame delivery stalls; dispatch also checks the deadline. Tests/native callers without an executor use
the same explicit deadline check. Already settled entries are reclaimed before checking per-node capacity.

Consuming an action at frame dispatch atomically checks and disarms its waiting timeout. That timeout does not limit
subsequent async work. An async owner can explicitly arm its own deadline using `set_deadline(deadline, executor)`.
Timer callbacks share only response state and race safely with completion, cancellation, and destruction. They do not
cancel side effects already started. The executor must outlive actions with timers attached to it.

Discarding a frame batch reports `cancelled`; ignoring a delivered action reports `internal_error`. Async owners must
complete, fail, or release their actions on shutdown. A consumed action may finish after its node is removed, but cannot
implicitly target a replacement. A disconnected client does not cancel accepted work, and requests are never replayed.

## Adding an action

Define the payload in `types/node_action_contracts.hpp` with `BOOST_DESCRIBE_STRUCT`, then add a
`node_actions::contract_s<Payload, Result>` entry with the node type, action name and payload type name to
`node_actions::contracts`. Contracts are SDK-independent and available in CEF-disabled builds too. Use the
contract's name in dispatch and its payload type with `get_typed_payload` in the dedicated handler. Current actions
return `std::nullptr_t`; new result types also need support in the TypeScript generator.

Described payloads require JSON objects, including empty contracts, and ignore unknown members. Required members
remain required. Mark a member with `json_member_defaulted<&Payload::member> = true` only when omission should use
its `Payload{}` initializer. This produces an optional, non-nullable TypeScript property. Explicit null still fails
unless the member is nullable. `std::optional<T>` retains its existing missing/null semantics. Additive fields need
backward-compatible defaults; changing a field's meaning is not made compatible merely by ignoring unknown keys.

Include `nodes/action.hpp`, route the name in `handle_action`, and implement a dedicated handler. Return `frame` for
frame-bound work and route it to a dedicated handler in `handle_frame_action`. Forward or move the entire action;
there is no separate completion or action plan to construct. Keep frame handlers nonblocking and latch work for later
lifecycle stages when GPU recording is required.

The reusable `NodeActionInterface` supplies a row of non-port buttons:

```ts
reload: () => new NodeActionInterface("Reload", "cef_browser", [
  { label: "Reload ↻", action: "reload" },
  { label: "Force ↻", action: "reload", payload: { ignore_cache: true } },
]),
```

The native generator exports payload interfaces and `node_action_contracts_s`, scoped by node type and action name.
`NodeActionInterface` uses this catalog to check button names and payload types. Payloads can be omitted only when the
contract accepts an empty object. Unknown object fields remain allowed; known field types are checked.
Custom controls use `requestNodeAction(ws, "cef_browser", nodeId, { action: "reload", payload: { ignore_cache: true } }, abortSignal)`
from `node_actions.ts`. It types the request and result from the same catalog; the server still resolves the actual
node type by ID. The underlying WebSocket helper adds the token,
limits outstanding waits, and cleans up on replies, disconnect, a ten-second timeout, or abort. Aborting cancels
only the local reply wait. The control aborts that wait on unmount and disables its buttons while waiting. Labels remain unchanged;
errors are logged to the console without inline feedback.
The control itself does not write an option; action-derived changes arrive through normal authoritative broadcasts.

## Browser reload

The browser accepts `reload` with the `browser_reload_payload_s` object contract. Omitted `ignore_cache` defaults
to false; a present value must be boolean. Unknown fields are ignored.
Configuration admission only recognizes the name and selects frame delivery. The dedicated frame-side reload handler
validates the payload and requires an enabled, ready session matching the frame's URL, size and frame rate. The owning
`session_request_s` posts reload to CEF's UI thread; the frame-consumer session API retains no lifecycle controls.
Only one reload task may be pending for a session. CEF `Reload()` uses normal cache behavior and
`ReloadIgnoreCache()` explicitly bypasses it.

Reload retains the browser and frame pool, cancels old page commands, and uses the existing navigation/capture-epoch
handling. Completed frames remain available during loading. Normal browser status reports loading, new captured
frames and failures. The action does not increment automatic restart counts, change options, or recreate a failed
session. CEF-disabled builds deserialize the same contract before rejecting valid reload requests as unavailable.

## Global settings actions

Refresh Fonts is available in Global Settings and uses the existing application-wide font registry command.
It is no longer repeated on individual text and teleprompter nodes.

Clear Browser Cache sends `clear_browser_cache` with the `clear_browser_cache_payload_s` object contract (normally `{}`,
with unknown fields ignored) to the application settings node (`$app`). It clears
CEF's shared HTTP cache for all browser nodes, without reloading pages or deleting cookies, local storage, or
service-worker storage. It also works with no active browser nodes. CEF-disabled/unavailable runtimes return
`unavailable`; concurrent clearing returns `busy`. The action acknowledges scheduling, and the settings node's
`browser_cache_clearing` status reports whether the asynchronous operation is still pending. SDK work and completion
run on CEF's UI thread. Scheduling starts on the configuration thread after the graph commit; it does not wait for
a frame boundary or access render-owned node state.

## Validation

`core_test` also checks action payload defaults, unknown fields, object/type rejection and typed enum failures.
Generator tests cover exported action contracts; `npm run build` checks the positive and negative TypeScript cases in
`web/tests/node_actions.type-test.ts`.

`core_test` covers configuration admission, atomic explicit settings, ordered best-effort action patches, connection-
derived settings, authoritative broadcasts and persistence, owned payloads, 500-node FIFO frame delivery, per-instance
capacity, replacement across frame cutoffs, asynchronous ownership, expiry, cancellation, reply exceptions, and typed
wire decoding.
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

Configuration-admission redesign validation:

- CEF-enabled and CEF-disabled native builds passed without compiler warnings; all 187 CTests passed in each build.
- All five existing WebSocket client tests passed without client or wire-contract changes.
- Both WebSocket integration variants passed with Vulkan validation enabled and no validation errors.
- Manager-level tests include 500-node FIFO delivery, settings/action batches, settings derived from connections,
  action-origin broadcasts, delayed expected-instance checks, and completion ownership across removal and shutdown.
