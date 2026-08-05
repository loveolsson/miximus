# Frame clock synchronization work plan

## Purpose

Miximus currently schedules the program timeline from a free-running `steady_clock`. DeckLink inputs and outputs already
observe stable device timing, but those observations only drive source selection and output presentation. This plan adds
an optional application-wide frame-clock discipline so a selected DeckLink node can align the program render cadence to
its physical frame boundaries.

The program clock remains robust when the selected source disappears. If no valid external observation arrives for
approximately two provider-frame intervals, Miximus enters holdover and immediately continues at the configured nominal
program rate. Missing sync must never block the render thread or stop graph evaluation.

This work is an extension of [frame timing and synchronization](frame-timing-and-synchronization.md). It does not replace
the existing input clock recovery or buffered output timing. Those systems normalize source frames and schedule output
frames respectively; frame-clock synchronization controls the global program evaluation cadence.

## Design rules

1. Exactly one frame-sync provider may discipline the program clock at a time.
2. Provider selection is a global application setting, not a checkbox on each media node.
3. Providers are identified by stable node ID and publish through a generic application service.
4. Every observation includes the provider's exact rational frame rate.
5. Sync is accepted only when the provider and configured program rates have a supported harmonic relationship.
6. Miximus owns one persistent program-clock object. Selecting a provider changes its discipline state; it never replaces
   the clock object used by the scheduler.
7. Input and output nodes are observation providers, not implementations of a scheduler clock interface.
8. `frame_context_s::target_time` remains in Miximus' absolute steady-clock domain.
9. External phase corrections are bounded and gradual; normal acquisition must not jump the program timeline.
10. Loss of observations enters free-running holdover without creating a new epoch.
11. Explicit provider changes, program-rate changes, or irrecoverable timestamp discontinuities create a new epoch.
12. SDK callbacks only publish bounded observations. They never evaluate the graph or wait for the render thread.

## Existing infrastructure

The implementation should extend the structures already present rather than establish a parallel timing system:

- `core::clock_source_i` currently abstracts time queries and deadline waits. It should be narrowed and renamed to
  represent only the injectable monotonic timebase.
- `core::frame_scheduler_s` owns frame identities, PTS, epochs, deadlines, lateness, and skipped evaluations.
- `core::frame_context_s::target_time` is the absolute time associated with rendered frames and buffered outputs.
- The reserved `$app` node owns global frame rate and output buffering settings.
- `app_state_s::frame_settings_s` is the immutable frame-boundary settings snapshot.
- `media::source_clock_estimator_s` already estimates affine rate and phase relationships.
- DeckLink input records stream time, hardware-reference timestamps, and callback arrival time.
- DeckLink output maps completion-reference timestamps into steady time for its physical playhead.
- `settings_option_s` and status-driven dropdowns already represent dynamic `{id, label}` selections.
- Stable node IDs and render-thread node destruction provide an appropriate provider identity and lifecycle boundary.

The important missing pieces are a generic provider registry, one persistent program-clock discipline, and a way for
the scheduler to adjust its steady-domain anchor without invalidating output target times.

## Application-level provider registry

Add a generic frame-sync registry owned by `app_state_s`. It belongs in `core` or `media`, not in the DeckLink registry,
because future NDI, audio-device, PTP, or other clock providers should use the same contract.

Each eligible node registers provider metadata under its node ID and receives a small publisher handle. Registration is
not a per-frame event. The handle points directly at a generation-specific registry entry so an SDK callback does not
perform a string-map lookup for every frame.

Removing or reconfiguring a provider invalidates its generation. A callback retained by an SDK may then fail to publish
safely without touching a destroyed node or a new node which happens to reuse the same ID.

The registry maintains a versioned list of choices for the UI:

```json
[
  {"id": "decklink-input-id", "label": "DeckLink input — Camera"},
  {"id": "decklink-output-id", "label": "DeckLink output — Program"}
]
```

Provider metadata should include node type, display label, current nominal rate, availability, and DeckLink reference or
signal-lock quality where applicable. Provider availability and external reference lock are distinct: an output's
hardware oscillator can provide cadence even when the card is not locked to an external reference.

## Observation contract

A published observation should contain at least:

```cpp
struct frame_sync_observation_s
{
    uint64_t      epoch;
    uint64_t      sequence;
    frame_rate_s  frame_rate;
    utils::flicks source_time;
    utils::flicks steady_time;
    utils::flicks frame_duration;
};
```

- `source_time` identifies the physical frame boundary in the provider's local clock domain.
- `steady_time` identifies that same boundary in Miximus' absolute steady-clock domain.
- `frame_rate` is the exact rational cadence provided by the source.
- `epoch` changes after device restart, format change, timestamp discontinuity, or other loss of continuity.
- `sequence` detects duplicates, missed observations, and out-of-order callbacks.

Publication must be bounded and non-blocking. A latest-sample channel with a sequence counter is sufficient; the render
scheduler does not need an unbounded history of callback events.

## Frame-rate compatibility

Compatibility must use exact rational arithmetic. Floating-point proximity is not sufficient for broadcast rates.

For the initial implementation, reduce the ratio between provider rate `S` and configured program rate `P`:

```text
S / P = a / b
```

Accept sync only when the reduced ratio is one of:

```text
1 / 1   exact cadence
2 / 1   provider supplies two boundaries per program frame
1 / 2   provider supplies one boundary per two program frames
```

This deliberately supports:

- 60 ↔ 30;
- 60000/1001 ↔ 30000/1001;
- 50 ↔ 25;
- any future exact-rate or two-times harmonic pair.

It deliberately rejects superficially close but different clock families such as 60 and 60000/1001, or 60 and
30000/1001. The allowed harmonic set can be extended later, but accepting arbitrary integer ratios before a real use
case would complicate phase selection and missing-observation policy unnecessarily.

When the provider runs at twice the program rate, all observations may contribute to rate estimation, while program
boundaries align with one consistent provider-frame parity. When the provider runs at half the program rate, the clock
discipline interpolates the program boundary between observed provider boundaries. In either case, the program PTS
duration remains the configured program duration.

An incompatible selected provider remains persisted but cannot discipline the scheduler. Runtime state reports
`incompatible` and the program free-runs until the rates become compatible.

## Global selection and UI

Add a string option to the `$app` settings node:

```text
frame_sync_source_id = ""
```

An empty ID, or a UI entry labelled `Internal`, selects the existing steady-clock behavior. A missing provider ID is
valid persisted configuration because hardware may not be attached when a project loads.

The `$app` node publishes the versioned provider-choice list as runtime status. The global settings panel uses the same
status-backed dropdown mechanism as device, monitor, font, and NDI selections. This makes exclusivity structural: the
authoritative application settings contain only one selected ID.

A graph-visible sync node is not needed. Frame cadence is application control state rather than a media dependency, and
it must continue operating regardless of which graph closure executes.

## Disciplined program clock

Miximus should have exactly one persistent `program_clock_s`. It always advances, uses `steady_clock` as its underlying
timebase, and may be disciplined by the one globally selected provider. Selecting, removing, or reacquiring a provider
changes state inside this object; it does not replace the clock held by the scheduler.

DeckLink input, DeckLink output, and future providers must not derive from a clock interface or become alternate clock
objects. They only register metadata and publish timestamped observations through generation-safe handles. This keeps
provider lifecycle and SDK callback ownership independent of the scheduler's lifetime.

The existing clock abstraction combines two concepts which should be separated:

1. a monotonic timebase which can report the current time and wait for an absolute deadline;
2. the program clock which decides the cadence and phase of program-frame targets.

Narrow and rename `clock_source_i` to a timebase interface such as:

```cpp
class monotonic_clock_i
{
  public:
    virtual utils::flicks now() const noexcept = 0;
    virtual void wait_until(utils::flicks time) = 0;
};
```

Only production steady time and deterministic fake time need implementations. `program_clock_s` consumes that timebase
and the provider registry:

```cpp
class program_clock_s
{
    monotonic_clock_i&     timebase_;
    frame_sync_registry_s& providers_;

    // Selected provider ID, affine estimate, lock state, and holdover state.
};
```

`frame_scheduler_s` retains frame identity, PTS, epoch, lateness, skip policy, and metrics. It asks the persistent
program clock for steady-domain frame targets and deadline waits. The program clock owns only cadence discipline and
provider-state transitions.

Changing only `clock_source_i::wait_until()` is not correct. `frame_scheduler_s` currently fixes a steady-domain anchor,
and buffered outputs consume `frame_context_s::target_time`. Sleeping at corrected times while continuing to publish
uncorrected target times would split render scheduling from output scheduling.

The scheduler or its clock controller must therefore apply a bounded correction to the steady-domain program anchor:

```text
nominal target   = anchor + frame index × configured duration
corrected target = nominal target + bounded phase correction
```

The resulting corrected target remains an absolute steady-clock value and is written to `frame_context_s`. DeckLink,
NDI, and screen outputs can therefore retain their current absolute-target-time contract.

`media::source_clock_estimator_s` contains reusable affine estimation logic, but the provider discipline should have a
name and interface appropriate to generic clocks rather than pretending it is an input-frame queue. Shared estimation
math should be factored without coupling the scheduler to source selection.

## Lock, holdover, and reacquisition

Expose an explicit runtime state:

```text
internal
unavailable
incompatible
acquiring
locked
holdover
discontinuous
```

Initial policy:

- Require two consecutive valid compatible observations before reporting `locked`.
- Treat an observation as missing when none has arrived for two provider-frame intervals.
- Enter `holdover` on that timeout and stop applying external phase/rate corrections.
- Continue immediately at the configured nominal program cadence from the current predicted boundary.
- Keep deadlines monotonic and do not wait indefinitely for a callback.
- On return, enter `acquiring` and slew phase back into alignment with a bounded per-frame adjustment.
- Do not create a new epoch for an ordinary brief lock loss or reacquisition.
- Create a new epoch for explicit provider changes, configured program-rate changes, provider epoch changes which cannot
  be reconciled, or phase discontinuities too large for bounded acquisition.

The timeout is measured using the provider rate carried by its observations. For a half-rate provider, two provider
frames span four program frames; this is intentional because provider liveness is defined in its own cadence.

## DeckLink input provider

For each valid captured input frame:

1. Read `GetHardwareReferenceTimestamp()`.
2. Subtract the reported duration to obtain the start-of-frame-on-wire boundary, matching the SDK example.
3. Relate that hardware value to absolute steady time using the DeckLink hardware-reference clock.
4. Publish the frame boundary, exact detected rate, source epoch, and sequence through the node's provider handle.

Frames marked `bmdFrameHasNoInputSource` do not refresh sync. Cable loss therefore reaches holdover naturally. Format
change updates the provider rate and epoch before new observations become eligible.

The current input hardware observation used by `timed_source_queue_s` remains responsible for mapping captured media
onto the program timeline. Publishing a program-clock observation is an additional use of the device timing, not a
replacement for source-frame selection.

## DeckLink output provider

The output callback already obtains `GetFrameCompletionReferenceTimestamp()`, derives the completed frame's physical
start time, and maps the device playhead into steady time. Publish that same physical observation through the provider
handle with the configured output-mode rate and output sequence.

This publication must not replace the output's existing `output_timeline_s`, timed queue, preroll, or repeat/drop logic.
The output still has to adapt buffered program frames to physical output slots even when it is also the selected program
clock provider.

An output provider is available while scheduled playback is producing valid completion timestamps. Stopping playback,
device removal, mode change, or repeated timestamp-query failure stops publication and eventually enters holdover.

## Frame-boundary integration

The selected source ID is read from the same immutable `$app` render snapshot as the frame rate and output buffer
settings. It is copied into `app_state_s::frame_settings_s` once per evaluation.

Before calculating the next frame target, the scheduler consumes the latest selected-provider observation, updates its
lock state and clock estimate through the persistent `program_clock_s`, and applies any bounded anchor correction. The
program-clock instance itself is never switched. The remainder of `tick_one_frame()` retains its existing preparation,
submission, execution, GPU completion, and node completion ordering.

Provider callbacks never access `node_manager_s`, mutate node state, or invoke the scheduler directly.

## Runtime status

Publish frame-clock status on `$app` at the existing throttled timing cadence:

- requested provider ID;
- active provider ID and label;
- provider type and exact rate;
- configured program rate and harmonic ratio;
- state (`internal`, `locked`, `holdover`, and so on);
- observation age in microseconds and provider frames;
- recovered rate ratio;
- phase error and applied phase adjustment;
- consecutive valid and missing observation counts;
- lock acquisitions, lock losses, holdover entries, and discontinuities;
- DeckLink signal/reference lock quality when provided as metadata.

These are runtime diagnostics, not a duplicate serialization of application settings.

## Implementation stages

### Stage A: generic registry and compatibility

- Add provider metadata, observations, generation-safe publisher handles, and the app-owned registry.
- Implement exact rational harmonic compatibility.
- Add deterministic tests for equal, double, half, and incompatible rates.
- Add tests proving stale publishers cannot affect a replacement registration.

### Stage B: hybrid clock discipline

- Narrow the existing clock interface to an injectable monotonic timebase with steady and fake implementations.
- Add one persistent `program_clock_s`; do not add DeckLink-specific or per-node clock implementations.
- Extend the scheduler/program-clock boundary so the steady-domain anchor can be disciplined safely.
- Preserve current internal-clock behavior bit-for-bit when no provider is selected.
- Implement acquisition, bounded correction, holdover, reacquisition, and discontinuity handling.
- Add fake-clock tests for monotonic targets and deterministic state transitions.

### Stage C: global setting and status

- Add `frame_sync_source_id` to `$app` and its frame-local settings snapshot.
- Publish dynamic provider choices and timing status.
- Add the status-backed selector to the global settings panel.
- Preserve unavailable IDs across project load and hardware absence.

### Stage D: DeckLink input publication

- Register input nodes as providers and pass generation-safe handles into capture callbacks.
- Convert valid hardware-reference frame boundaries into steady time.
- Publish detected rate and epochs across format changes.
- Verify cable loss enters holdover without stalling or cooling the input.

### Stage E: DeckLink output publication

- Register output nodes as providers and reuse completion-reference observations.
- Publish output mode rate and lifecycle availability.
- Verify output restart, mode changes, and device removal enter holdover safely.
- Preserve all existing buffered-output timing and transfer ownership.

### Stage F: hardware and endurance validation

- Select input and output providers independently on the DeckLink loopback graph.
- Test exact-rate, 60↔30, 60000/1001↔30000/1001, and incompatible-rate configurations.
- Pull and reconnect input cables while monitoring render cadence and output queues.
- Restart or remove the selected output without blocking the render thread.
- Run controlled render-delay tests while externally locked and during holdover.
- Run a long soak and verify phase error remains bounded without periodic jumps or accumulated drift.

## Exit criteria

- Internal free-running behavior is unchanged when no provider is selected.
- The scheduler retains one `program_clock_s` for its lifetime; provider selection never replaces that object.
- Media nodes only publish observations and cannot independently acquire clock authority.
- Only one globally selected provider can discipline the scheduler.
- Compatible exact, double-rate, and half-rate providers lock deterministically.
- Incompatible rates never influence the program anchor.
- Two missing provider frames enter holdover without blocking or discontinuity.
- Holdover targets remain monotonic and continue at the configured nominal rate.
- Reacquisition applies bounded correction rather than a visible timestamp jump.
- DeckLink callbacks remain bounded and node destruction remains race-free.
- `frame_context_s::target_time` stays in the absolute steady-clock domain used by every buffered output.
- Input source selection and output presentation timing retain their existing responsibilities and behavior.
- Status distinguishes selected, active, locked, holdover, unavailable, incompatible, and discontinuous states.
