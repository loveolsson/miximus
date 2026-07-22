# Frame timing and synchronization plan

## Status and purpose

This document describes the planned transition from the current fixed-rate render loop to a PTS-aware, buffered,
broadcast-oriented frame pipeline. It is a design and migration plan, not a description of behavior that has already
been implemented. The current runtime contracts remain documented in [architecture.md](architecture.md) and
[gpu-and-media.md](gpu-and-media.md).

The design has five central rules:

1. Miximus owns one explicit program timeline and one active clock source.
2. Source timestamps identify positions on source-local timelines; they are not absolute Miximus timestamps.
3. Inputs adapt their local timelines to the program timeline, while outputs buffer frames for scheduled playout.
4. Every input remains hot and advances continuously, independently of whether it contributes to this frame's output.
5. A batch of option mutations becomes visible atomically at one frame boundary; addressing a future frame or PTS is
   deferred.

## Goals

- Support rational broadcast frame rates such as 24, 25, 30, 50, 60, 24000/1001, 30000/1001, and 60000/1001.
- Keep media PTS separate from wall-clock deadlines and callback arrival times.
- Tolerate short render-time variance through bounded output buffering.
- Skip obsolete program frames without accumulating clock drift.
- Select an appropriate input frame for each program PTS instead of consuming the most recently completed frame.
- Adapt independent input clocks using stable clock recovery, hysteresis, and deliberate repeat/drop decisions.
- Keep every input capturing, decoding, advancing bounded queues, and producing transferable frames continuously. Graph
  demand controls consumption and painting, never whether an input is hot.
- Give active graph work a submission pass before its execution pass so transfers and other asynchronous work can begin
  before any consumer needs to resolve them.
- Keep decode, blocking SDK work, CPU image processing, and transfer execution off the render thread. The render thread
  only reaches the mandatory synchronization point for an exact selected transfer during node `execute()` after every
  active source has had an opportunity to submit work.
- Support DeckLink, NDI, screen presentation, and future FFmpeg sources through common timing contracts while
  preserving their different native memory and clock models.
- Allow external controllers to submit a batch of option changes that takes effect atomically at one frame boundary.
- Expose enough timing and queue metrics to diagnose late frames, drift, underruns, and sustained overload.

## Non-goals for the first migration

- Rendering one graph independently at several unrelated program frame rates.
- Interlaced video; the program timeline and all supported inputs and outputs are progressive-only.
- Seamless switching between clock sources without flushing and re-prerolling affected pipelines.
- Turning the graph into a generally parallel task executor.
- Blocking the render thread indefinitely to preserve every input or output frame.
- Treating callback arrival time as a precise media clock.
- Demand-driven suspension of input capture, decode, or transfer pipelines.
- PTS-addressed or future-frame-addressed option transactions.
- External clock synchronization and audio processing in the initial implementation.

The initial model should have one configured program format. Outputs at another cadence must use an explicit
rate-adaptation policy. Supporting several independent program timelines later would require separate evaluation
caches and potentially multiple graph evaluations for the same logical scene.

## Current limitations

The fixed loop-local timing arithmetic has been replaced by an anchor-based scheduler with an internal steady clock,
explicit epochs, monotonic frame identities, coordinated PTS gaps, and a replaceable late-frame policy. The current
provisional policy permits an evaluation up to one frame late and skips every older evaluation in one decision. Input
selection and output buffering are not yet PTS-aware; those remain the important limitations addressed by later
stages.

The current graph frame performs these operations:

1. update the stable render snapshot;
2. call `prepare()` on every node;
3. determine demanding sinks and their structurally active upstream closure;
4. call `submit()` once on every node in that closure;
5. lazily execute the dependencies of every demanding sink;
6. call `glFinish()` globally;
7. call `complete()` on every node.

This preserves a stable graph, advances every node through `prepare()`, gives active sources an opportunity to submit
work before painting, and keeps render-thread GL destruction. The remaining limitation is that media inputs do not yet
use the submission pass to select and start a PTS-aligned transfer ticket that execution subsequently awaits.
`prepare()` also still combines configuration maintenance, device lifecycle, status updates, queue advancement,
allocation, and per-frame acquisition in several existing nodes.

The transfer services use monotonic versions or arbitrary integer tags and publish the latest completed transfer.
Those are useful implementation details but do not identify the media frame for which a transfer was requested.
Texture and framebuffer interfaces similarly expose raw pointers without PTS, epoch, or readiness metadata.

## Program timeline

### Program format

The application should own a program-format value containing at least:

- rational frame-rate numerator and denominator;
- nominal frame duration in `utils::flicks`.

There is no global program resolution. Framebuffer paths continue to determine their own dimensions, and inputs and
outputs independently convert between their native cadence and the one global graph-evaluation rate. Input queue
targets, output buffer depths, missing-frame policy, and late-frame policy are application settings rather than fields
that define the program format itself.

The scheduler owns the current epoch separately. An epoch identifies one continuous mapping between program PTS and
the scheduling clock; it is runtime timeline state, not a user-authored format field.

`utils::flicks` remains the media-time representation because its time base exactly represents common broadcast
rates. The rational rate must still be retained as first-class format information rather than reconstructed wherever
an SDK requires it.

### Frame context

Each graph evaluation should receive one immutable frame context. A likely initial shape is:

```cpp
struct frame_context_s
{
    uint64_t      frame_number;
    uint64_t      epoch;
    utils::flicks pts;
    utils::flicks duration;

    steady_time target_time;
    steady_time render_deadline;

    bool discontinuity;
};
```

The concrete steady-clock type and placement will be decided during implementation. The important distinction is:

- `pts` identifies media on the program timeline;
- `target_time` maps that PTS to the active scheduling clock;
- `render_deadline` is the latest useful completion time;
- `frame_number` names a graph/configuration boundary;
- `epoch` invalidates timestamp relationships after a restart, seek, format change, or clock discontinuity.

There is no single `playout_time` in the frame context because each output owns its own cadence, buffering, and
presentation mapping.

### Scheduler

The scheduler must derive deadlines from a stable anchor:

```text
target_time(frame N) = anchor_time + N * frame_duration
```

It must not calculate each next deadline from the time the previous frame happened to finish. This prevents render
jitter from becoming clock drift.

The late-frame policy should distinguish:

- a frame that is slightly late but still useful to buffered outputs;
- a frame whose output deadline has passed and should be skipped;
- several obsolete frames, which must all be advanced in one decision;
- sustained overload, which should be visible as degraded runtime status rather than an unbounded backlog.

The exact useful-late threshold is deliberately not fixed by this plan. The working target is to execute evaluations
that are no more than approximately one frame late and begin skipping obsolete evaluations beyond that point, but the
final rule must come from deterministic simulation, industry practice, hardware testing, and possibly configuration.
Implement it behind a scheduler policy rather than spreading threshold checks through nodes or outputs.

When a program frame is skipped, its frame number, PTS, input selection, and output accounting must all advance
consistently. Downstream queues observe monotonic PTS with explicit gaps and must not be forced into a catch-up cascade.
The anchor never moves merely because rendering was late. Skipped evaluations do not synthesize calls to node
lifecycles; `prepare()` on the next real evaluation drains each continuously running input queue up to the new program
position.

## Source-local clocks and alignment

### Timestamp domains

An input can expose several timestamps that must not be conflated:

- source stream PTS, normally relative to a source-defined epoch;
- a capture-device hardware-reference timestamp;
- host callback arrival time;
- the Miximus program PTS.

Source PTS orders media and describes source cadence, but its zero point is arbitrary. Callback arrival time includes
driver and thread-scheduling jitter and is primarily a latency diagnostic. A hardware-reference timestamp is the
preferred observation for relating device capture to a stable clock when it is available.

### Adaptive mapping

Each asynchronous input needs an estimator conceptually mapping:

```text
program_pts = source_pts * rate_correction + phase_offset
```

The estimator must determine both rate and phase from timestamp progression over multiple frames. It should:

- establish an initial phase using a configured target input-buffer depth;
- estimate deltas rather than compare absolute PTS values;
- constrain recovered rate near the source's declared nominal rate;
- smooth corrections over time instead of following individual callback timing variations;
- reject outliers;
- use buffer occupancy as an additional drift signal;
- create a new source epoch after timestamp jumps, backwards movement, format changes, signal loss, reconnects, or
  seeks.

This is a clock-recovery loop, similar in purpose to a small PLL. A source nominally running at 60000/1001 may be a
few parts per million faster or slower than the program clock. The estimator lets the timelines appear aligned over
short intervals while gradually tracking the real difference.

Video cannot be continuously resampled in time. Independent clocks eventually require a discrete correction:

- repeat the selected source frame when the source falls behind;
- discard a source frame when it gets ahead.

Selection must use hysteresis so nearly aligned clocks do not alternate rapidly between drops and repeats. The input
buffer should remain bounded around a target depth rather than grow or drain indefinitely.

### Selecting a frame

For a requested program PTS, the input should select the source frame whose adaptively mapped presentation interval
best contains that sample point. A simpler first implementation may select the newest mapped source frame at or
before the requested PTS, retain it for repetition, and discard older frames.

Every selection should produce a result that distinguishes at least:

- a newly selected frame;
- an intentional repeat;
- missing signal/black fallback;
- discontinuity;
- transfer failure.

## Clock sources

The frame scheduler should depend on a clock-source interface rather than sleeping directly in `main.cpp`. The initial
implementation uses only an internal free-running `steady_clock`. The interface must not assume that implementation so
external clocks can be added later, but DeckLink reference/input clocking and automatic clock switching are outside the
initial action plan.

There is exactly one authoritative program clock. Normal inputs adapt to it independently. If an input is explicitly
selected as the master, its callback must publish clock observations to the scheduler through a bounded/thread-safe
bridge; SDK callbacks must never invoke graph evaluation directly.

NDI FrameSync is designed to adapt NDI inputs to the receiver's clock and should normally follow the program timeline,
not become its hard clock source.

Changing the program frame rate creates a new timeline epoch. This is an initial-setup operation, not a normal live
control: it may flush input selection, restart output scheduling, repreroll devices, and cause a multi-second glitch.
Future clock-source changes may use the same discontinuity mechanism.

## Node lifecycle and active graph

### Per-frame phases

`prepare()` remains an all-node phase. It must run once for every node on every program evaluation, including nodes
outside the rendered closure. Input nodes use it to drain or advance bounded source queues, update clock recovery,
discard obsolete media, retain the current candidates, process asynchronous lifecycle results, and keep their source
pipeline ready for an immediate graph switch. It must not wait for device control or a host/GPU transfer.

After all-node preparation, active sinks define an upstream closure. That closure is traversed twice, with once-per-node
caching in each pass:

1. submission traversal: select and park the source frame for the current program PTS and initiate or associate any
   transfer or asynchronous work needed by active nodes;
2. execution traversal: resolve the parked work within the frame budget, execute dependencies, and paint outputs.

The first traversal is not a replacement for `prepare()`. It creates overlap between active asynchronous work and later
consumption. Callback-driven input transfers may already be submitted before either traversal; their submission step
associates the correct ticket with this program frame. Pull-driven inputs may issue their current request during
`prepare()` or submission, depending on how they remain continuously productive without blocking.

`complete()` continues to run once for every node after active execution so frame reservations, status, and deferred
retirement progress consistently. Configuration and device lifecycle work should be event-driven where possible, and
blocking SDK calls remain on device-control workers.

The intended frame order is therefore:

1. apply the immutable application-settings snapshot and pending option batch at the frame boundary;
2. update the stable node snapshot;
3. call `prepare()` on every node;
4. discover demanding sinks and their active upstream closure;
5. run the submission traversal over that closure;
6. run the execution traversal over that closure;
7. retire frame-local GPU use and call `complete()` on every node;
8. publish aggregated status without blocking.

### Discover render demand

The manager should first determine which sinks need a frame. Examples include:

- enabled DeckLink outputs;
- enabled NDI outputs, regardless of current receiver count;
- enabled screen outputs;
- future encoders, recorders, and explicit preview consumers.

The manager traverses connections upstream without painting and produces the active render closure. Only nodes in this
closure receive submission and execution, but every node still receives `prepare()` and `complete()`. This replaces the
current `must_run` boolean with a more explicit sink-demand contract without making input readiness demand-driven.

Inputs are always hot. DeckLink capture, NDI sampling, future file playback/decode, queue advancement, and their normal
transfer pipelines continue even when their texture is not selected by an output this frame. An implementation may
avoid a graph-specific conversion or transient render target that is provably unnecessary, but it must never stop or
cool an input merely because the current render closure does not include it. Switching the graph on the next frame must
not wait for device startup, decoder startup, or a newly warmed source queue.

Here, hot applies to an input that is enabled or playing according to its own explicit options. An explicit disable,
stop, close, or device reconfiguration may stop its pipeline; render demand may not.

### Prepared frame tickets

An input's queue advancement and submission should produce a parked ticket rather than expose a partially ready
texture. The ticket should carry the media identity and a readiness state such as:

- ready;
- transfer submitted with a shared GPU fence;
- reserved but not submitted;
- missing;
- discontinued.

Callback-driven devices such as DeckLink continuously accept frames into bounded host-visible reservations. All-node
`prepare()` advances the source queue. The active submission traversal selects the exact frame assigned to the current
program PTS, parks its ticket, and starts that frame's transfer. Pull-driven sources such as NDI FrameSync or a decoder
likewise keep their production queues moving from `prepare()`, while submission starts the selected host/GPU transfer.
Both paths retain the selected frame and its storage until frame retirement.

Execution awaits the exact ticket selected for this program PTS. Transfer lateness therefore makes the program frame
late; it must never silently replace the selected frame with an older ready frame. Bounded output queues are responsible
for absorbing ordinary render-time variance. A repeat, missing frame, or black frame is chosen deliberately by the
source timing policy before submission when no newer source frame is appropriate. Once selected, that decision is
authoritative for the evaluation. Device loss, cancellation, and transfer failure still require finite operational
failure handling so shutdown cannot hang, but they are errors rather than normal deadline-based fallback decisions.

Transfer APIs should replace bare `version`/`tag` semantics at media-facing boundaries with structured metadata such
as:

```cpp
struct media_frame_id_s
{
    uint64_t      epoch;
    uint64_t      sequence;
    utils::flicks pts;
    utils::flicks duration;
};
```

Internal monotonically increasing slot versions may remain where they are useful for ownership and stale-result
protection.

### Graph texture contract

Timing normalization ends at the source node boundary. When an input publishes a `texture_s*` for execution, that
texture is its image for the current global program PTS. Downstream nodes must not care whether the source selected a
new frame, repeated its previous frame, dropped an obsolete frame, or recovered from a discontinuity. They execute
normally because options, animations, other inputs, and output targets may still have changed.

Source timing, fallback state, and transfer readiness therefore remain private to the input synchronizer and its
prepared ticket. Program timing comes from the global frame context, while output queues associate the completed
program result with that context's PTS. The existing texture and framebuffer interface types do not need to become
frame-valued interfaces as part of this work.

Textures and framebuffers may eventually carry separate descriptive source information, for example so a multiviewer
can derive an input label. That is a media-metadata feature rather than a synchronization contract. It should be
designed independently and must not cause ordinary nodes to branch on whether a texture was repeated or when it was
originally produced.

## Input integrations

### DeckLink input

For each captured frame, retain:

- `GetStreamTime()` and frame duration;
- `GetHardwareReferenceTimestamp()` when available;
- callback arrival time for diagnostics;
- input format, no-input-source state, colorspace, sequence, and source epoch;
- the upload reservation/version used by the transfer service.

The DeckLink synchronizer maps stream time into the program timeline using hardware-reference observations where
possible. Capture continuously fills bounded host-visible reservations regardless of current graph demand. All-node
preparation advances the timed queue; active submission selects the appropriate reservation for the requested program
PTS and starts its upload; execution awaits and resolves that exact ticket without asking the device to capture again.

The custom allocator is a long-lived capture-session resource, not a per-program-frame allocator. DeckLink may retain
and reuse an `IDeckLinkVideoBuffer` object across many captured frames. Each `StartAccess(bmdBufferAccessWrite)` begins
a new SDK write cycle and must acquire a fresh one-shot upload lease; the frame callback records timing metadata and
submits that lease without a full-frame copy. A completed texture has an independent lifetime and may be retained as
the input's repeated frame until a newer selected frame replaces it.

Format changes, device removal, and node destruction are asynchronous session transitions. The callback records the
transition and posts control work; it does not stop streams, wait for allocator references, or destroy GL resources
itself. Ordinary signal loss should produce missing/no-source frames without synchronously tearing down the session.
When reconfiguration is required, the serialized device-control executor stops capture, the render side releases its
retained texture without waiting, and the old allocator drains before its transfer stream is retired and a replacement
pool is created. Old and new full-size pools must not coexist merely to make reconfiguration easier.

If a DeckLink input and output share a hardware/reference domain, that relationship can provide a stronger alignment
than host callback arrival time. It must still be represented explicitly rather than inferred from equal numeric PTS
values.

### NDI input

Keep NDI FrameSync. Its documented purpose includes adapting independent sender clocks for video and audio mixing.

Every NDI input should keep its coalesced sampling/request stream active from all-node preparation. The capture worker
calls `NDIlib_framesync_capture_video()`, copies into an unsubmitted upload lease, and tags the result with the
associated source observation. Active submission selects the appropriate reservation, parks its ticket, and starts its
upload rather than being responsible for keeping the receiver warm.
The returned NDI timecode, receive timestamp, format, and frame rate should be retained for diagnostics and
discontinuity detection even though FrameSync performs the primary time-base correction.

Calling FrameSync from a worker is appropriate because host memory copying should stay off the render thread. The
request queue must be bounded and obsolete requests must be dropped deliberately.

### FFmpeg input

File and stream nodes should use dedicated or managed demux/decode workers with bounded decode-ahead queues. They
should retain packet PTS/DTS, `AVFrame::best_effort_timestamp`, duration, and stream time base, then rescale into
`utils::flicks`.

The source epoch changes on seek, loop, reopen, and discontinuity. Variable-frame-rate media must be selected by
timestamp rather than an assumed frame rate. A playing file input continues demux and bounded decode-ahead when it is
outside the current render closure. Only explicit playback state such as pause or stop may cool it; graph switches must
not implicitly alter its media timeline or readiness.

Hardware-decoded surfaces should enter the same prepared-ticket and transfer contracts without forcing CPU readback.

## Output scheduling and buffering

Outputs consume frames by intended presentation PTS rather than simply taking the latest completed transfer. Every
output queue must be bounded and define overflow, underrun, repeat, and shutdown behavior.

### DeckLink output

Preserve scheduled playback and callback replenishment, but associate each rendered/downloaded frame with its intended
DeckLink schedule time. Maintain explicit low, target, and high queue watermarks. On underrun, repeat the previous frame
or schedule black according to policy; discard a rendered frame that arrives after its useful scheduling point.
Repetition must explicitly schedule the retained frame for each required output interval. Do not depend on undocumented
behavior for gaps between DeckLink schedule timestamps.

Each scheduled frame must retain the download lease that backs its `IDeckLinkVideoBuffer` until DeckLink reports frame
completion. Do not copy completed downloads into a second DeckLink-owned frame buffer. The SDK completion callback may
poll completed leases and replenish the bounded schedule, but it must not make a GL context current or wait for a GPU
transfer. Transfer workers own their GL contexts and completion fences.

Playback start, stop, mode changes, and failure recovery belong to the serialized device-control executor. Shutdown is
a state transition: unregister scheduling, request `StopScheduledPlayback()`, and retain the callback, device, scheduled
frames, and their leases until `ScheduledPlaybackHasStopped()` or a bounded device-loss timeout. Node destruction must
remain non-blocking on the render thread even though normal removal may visibly interrupt output.

Use `GetBufferedVideoFrameCount()` and completion results to report queue depth, completed frames, late display,
drops, flushes, repeats, underruns, and preroll state. The output hardware/reference clock should be available as a
program-clock source. Reference lock and the resolved incoming reference mode are distinct from the configured output
mode and should be reported separately. SDK bitmask/FourCC values are implementation details: interpret flags as
bitmasks and resolve known modes to user-facing names, using `Unknown` when the SDK cannot identify one.

### NDI output

Keep `clock_video = false` while the Miximus scheduler owns cadence. The worker should request the frame for each NDI
send PTS, not consume an arbitrary latest result. Missing frames should be repeated deliberately and counted. NDI
timecode should be derived from program PTS, and future audio must use the same program timeline.

An enabled NDI output continues demanding upstream frame work even with no receivers. Receiver attachment must not
cause a sudden change in render load or reveal a graph that cannot sustain its configured outputs. The sender may skip
the download and network send while no receiver is present, but the upstream graph remains active.

### Screen output

Screen output is a scheduled, buffered output. Its presentation worker should derive its cadence from the selected
monitor's refresh rate and, where the platform exposes it, actual presentation feedback. The render side should queue
frames with their intended program PTS into a bounded set of shared render slots and maintain explicit preroll and
queue watermarks. It must not simply draw whichever frame happens to be newest when the display thread wakes up.

At each display presentation point, the output selects the queued program frame appropriate for that target. When the
display and program rates differ or rendering misses a deadline, it deliberately repeats the retained frame or drops
an obsolete queued frame according to the same policy used by other timed outputs. Queue overflow, underrun, repeat,
drop, late presentation, and measured presentation cadence should be reported as status.

The display context owns presentation and waits only through GPU-side synchronization. A shared render slot is not
returned to the producer until the display context has finished sampling it. Window visibility or compositor behavior
must not make the program render thread block; platform presentation stalls are contained by the bounded output queue.

## Application settings

Global application settings should initially behave like the option state of a node with a reserved ID, without being
an actual graph node. Reuse the established option-default, normalization, correction, read, update, and broadcast
patterns where practical, but keep the state outside `node_manager_s::nodes_` so it cannot be traversed, connected,
created, removed, or rendered.

The reserved ID and state belong in one shared native definition. Real-node creation must reject the reserved ID.
Configuration should persist the state as a top-level application-settings object rather than a fake entry in the
node array. The WebSocket update path may address it like node options, and a future system-settings view can consume
the same metadata without displaying a Baklava node.

The first settings include the rational global frame rate and configurable policy values for input queue targets,
output buffer depths, missing-frame timeout, and late-frame handling. Exact defaults are deliberately provisional and
will be tuned. A frame-rate change creates a new epoch and may restart timing-dependent integrations. The render thread
reads one immutable application-settings snapshot for the whole frame.

## Atomic option batches

Batched option changes are useful for external control but are independent of source PTS alignment and are not on the
critical path for the first timing milestones. The first transaction form contains a transaction ID and an ordered set
of option updates only. It has no target PTS or target frame number.

The configuration side normalizes and validates the entire batch before enqueueing it. If any update is invalid, the
whole batch is rejected. The render thread applies an accepted immutable batch at its next frame boundary, before
`prepare()`, so every update becomes visible in the same evaluation. Accepted batches retain FIFO ordering. Existing
single-node updates may remain on their current path until this independent feature is implemented.

Node creation/removal, connection changes, candidate-graph validation, graph revisions, and explicitly scheduled
future-frame transactions are later extensions. They must not complicate the first option-batch implementation or the
initial timing scheduler.

## Threading and ownership

The intended roles are:

- scheduler/render thread: frame boundaries, all-node preparation/completion, active submission/execution traversals,
  root GL submission, and application of pending option batches;
- configuration thread: parsing, validation, and immutable option-batch construction;
- SDK callback threads: bounded capture/timing observations and ownership handoffs only;
- device-control workers: blocking device start, stop, reconnect, and mode changes;
- decode/CPU workers: demux, decode, font rasterization, and CPU image processing;
- GL transfer workers: shared-context upload/download submission and fences;
- output workers/SDK callbacks: hardware/network scheduling from bounded PTS-aware queues.

No background object may destroy GL resources directly. Existing render-thread node destruction and service-owned
deferred cleanup on the owning GL worker must be preserved. Device callbacks and control tasks may release SDK/COM
references, but transfer leases must flow back to the transfer service for GL cleanup. Every queue requires a capacity,
ownership transition, stale-epoch rejection, and shutdown procedure. A node's asynchronous stop state must retain all
callback, device, allocator, and queued-frame objects that the SDK can still reference.

The global `glFinish()` can remain during early migration, but the end state should associate GL sync objects with the
resource or transfer slots whose reuse they protect. OpenGL fences belong to a command stream rather than intrinsically
to a resource, so one frame-completion fence may safely guard several resources last used by that frame.

## Status and diagnostics

The scheduler should report:

- program rate, frame number, PTS, epoch, and clock source;
- render start/end time, render duration, deadline margin, and lateness;
- prepare, closure-planning, submission, execution, GPU-finish, and completion durations;
- demanding-sink and submitted-closure node counts;
- skipped frames and sustained-overload state;
- active settings revision and pending/applied option-batch counts.

Each input should report:

- source nominal and recovered rate;
- estimated phase/latency and buffer depth;
- selected source PTS and resulting frame age;
- received, selected, repeated, dropped, missing, and discontinuous frames;
- transfer submission/readiness latency and allocation failures.

Each output should report:

- scheduled PTS and buffer depth against target;
- submitted, presented, repeated, dropped, late, and underrun frames;
- preroll and clock/reference state;
- transfer latency and allocation failures.

High-rate values should be aggregated or rate-limited before status publication. The existing delta status registry
should remain the transport mechanism.

## Audio compatibility

Video is the first implementation target, but the timeline must not preclude audio. Audio frames represent continuous
sample ranges rather than single video instants, and an audio device may eventually be the most stable program clock.
Program PTS, epochs, clock recovery, output preroll, and transaction boundaries must therefore remain meaningful at
audio sample precision.

NDI FrameSync can dynamically resample received audio to the local clock. Other audio inputs will need equivalent
sample-rate conversion and bounded buffering rather than video-style discrete repeat/drop alone.

## Migration stages

Each stage must remain buildable and runnable and should be committed independently after its exit criteria pass.
Do not combine the node lifecycle change with a hardware-node migration.

For each stage:

1. audit the affected current contracts before editing;
2. add deterministic coverage before or with behavioral changes;
3. preserve compatibility adapters until every current caller has migrated;
4. run the normal build, focused tests, formatting, and `git diff --check`;
5. perform hardware/runtime validation where listed;
6. update this document with measured decisions and mark the stage complete only after its exit criteria pass.

Stages 1 through 4 form the timing foundation and are sequential. Hardware integrations then migrate one at a time so
the application remains usable and regressions have a narrow source.

### Stage 1: application settings and timing primitives

**Status:** Complete

Implemented in the first timing migration. The current loop intentionally remains unchanged apart from consuming the
configured cadence. Focused tests cover exact integer and 1000/1001 rates, canonicalization, invalid settings, and
render-snapshot revisions. A real hardware-graph run verified legacy configuration migration, schema-2 persistence,
the default 60 fps graph, and graceful timed shutdown.

Deliverables:

- Add the reserved-ID, node-like application-settings state outside the graph.
- Persist it as a top-level configuration object and migrate existing configurations to the default 60 fps setting.
- Add an exact rational frame-rate type, derived `utils::flicks` duration, frame number, epoch, and immutable
  `frame_context_s`.
- Pass the context through `app_state_s` without changing current scheduling behavior.
- Expose settings and timing values through status/snapshot paths without adding a fake Baklava node.

Exit criteria:

- Existing configurations load unchanged and save with the new application settings.
- Integer and 1000/1001 rates round-trip exactly.
- Every node in one evaluation observes the same immutable settings revision and frame context.
- The existing default graph still runs at 60 fps.

### Stage 2: deterministic clock and scheduler

**Status:** Complete

Implementation and deterministic validation are complete. `clock_source_i` supplies the production steady clock and
test clocks; `frame_scheduler_s` owns the fixed anchor, program identities, epoch changes, deadlines, skip decisions,
and overload accounting. GoogleTest simulations cover 100,000-frame integer and 1000/1001 timelines, useful-late and
multi-frame overruns, policy comparisons over the same recorded workload, sustained overload, and live rate changes.
The hardware graph ran cleanly at 60 fps, accepted a live 60000/1001 epoch change, and was visually verified to remain
continuous and smooth across DeckLink loopback, NDI, screen output, and animated generators.

Deliverables:

- Introduce a clock-source interface with production `steady_clock` and deterministic fake implementations.
- Replace the loop-local 60 Hz arithmetic with an anchor-based scheduler.
- Introduce a replaceable late-frame policy, skip accounting, and scheduler status metrics.
- Start with a measured provisional policy near the one-frame-late goal; do not expose it as a permanent contract.

Exit criteria:

- Long simulations of integer and 1000/1001 rates do not accumulate deadline or PTS drift.
- Small and multi-frame overruns produce bounded work, monotonic PTS gaps, and no rapid backlog catch-up.
- Fake-clock tests can reproduce every skip decision exactly.
- Candidate policies can be compared from recorded traces before their thresholds become defaults or settings.
- Runtime status makes lateness and sustained overload visible.

### Stage 3: all-node preparation and two active traversals

**Status:** Implemented and hardware-verified

Deliverables:

- Preserve once-per-frame `prepare()` and `complete()` calls for every node.
- Determine demanding sinks and the active upstream closure after preparation.
- Add a once-per-node submission traversal before the existing execution traversal.
- Give nodes a small explicit hook for submission; keep the existing execution contract during migration.
- Replace `must_run` with explicit sink demand and remove or redefine the unused `wait_for_sync` trait.
- Instrument phase duration and ordering before moving real transfer behavior into the new hook.

The initial closure is deliberately structural: it follows every connected upstream edge from a demanding sink.
Execution remains lazy and may visit a smaller subset when a node selects among inputs at runtime. Future nodes that
submit expensive branch-specific work must expose enough routing information to prune unused candidates rather than
assuming every structurally reachable branch is selected.

Exit criteria:

- Tests prove every node prepares and completes even when outside the active render closure.
- Tests prove active nodes submit before any active node executes, with once-per-pass behavior for shared subgraphs.
- Existing graphs render identically before hardware integrations use the submission hook.
- Switching an output path never starts or warms an input device, decoder, queue, or transfer service because they are
  already hot.

### Stage 4: timed source queues and prepared tickets

**Status:** In progress

Deliverables:

- Add source epoch, sequence, source PTS, duration, arrival observation, and readiness to media-facing transfer results.
- Implement bounded timed queues and selection APIs instead of `consume_latest()` at input boundaries.
- Implement the source-to-program clock estimator, hysteretic repeat/drop selection, and discontinuity reset behavior.
- Let submission park and start the exact ticket for the current frame, then have execution await that same ticket.
- Keep raw texture/framebuffer graph interfaces unchanged; timing remains private to the source boundary.

Exit criteria:

- Fake sources with jitter, arbitrary PTS origins, drift, rate mismatch, and discontinuities select deterministic frames.
- Queues remain bounded and explicitly account for drops, repeats, missing frames, transfer lateness, and failures.
- A repeated source frame still permits normal downstream execution.
- Tests prove a late selected transfer is never replaced by a previously ready frame.
- Operational cancellation and failure paths are finite even though normal transfer lateness does not change selection.

### Stage 5: DeckLink scheduled output

**Status:** Pending

Deliverables:

- Associate each rendered/downloaded result with program PTS and its DeckLink schedule interval.
- Add explicit configurable preroll/target watermarks and cadence conversion from program rate to output rate.
- Explicitly schedule retained-frame repeats; never create timestamp gaps expecting the SDK to fill them.
- Preserve zero-copy download-lease-backed frames, completion ownership, device-control serialization, and asynchronous
  shutdown.

Exit criteria:

- Fake-output tests cover preroll, cadence conversion, late completion, repeat, underrun, and bounded queue behavior.
- Hardware output remains continuous under short render jitter and reports accurate completion/queue metrics.
- Device removal and node removal do not block the render thread or release a scheduled lease early.

### Stage 6: DeckLink timed input

**Status:** Pending

Deliverables:

- Attach stream time, duration, hardware-reference observation, arrival time, format, and source epoch to every submitted
  upload lease.
- Keep capture and upload submission continuous for every captured frame, regardless of render demand.
- Advance the timed source queue from all-node preparation and select its current ticket during active submission.
- Preserve custom-buffer reuse, fresh `StartAccess()` leases, last-frame retention, serialized reconfiguration, and
  allocator-drain ordering.

Exit criteria:

- Independent-rate and jitter tests show stable alignment with deliberate, hysteretic repeat/drop decisions.
- Switching the graph to the input requires no capture/transfer warm-up.
- Format changes, cable loss, reconnect, and removal remain non-blocking and memory-bounded.
- The local DeckLink output-to-input loopback runs continuously as a lifecycle/ownership soak test.

### Stage 7: NDI input and output

**Status:** Pending

Deliverables:

- Keep every NDI input's FrameSync sampling and upload stream hot from all-node preparation.
- Tag results with requested program identity and retain NDI timing observations for diagnostics/discontinuities.
- Make NDI output consume program-PTS results at its configured cadence, explicitly repeat missing frames, and derive
  timecode from the program timeline.
- Stop NDI output render demand when no receiver is connected without cooling any NDI input.

Exit criteria:

- Rate mismatch and injected network/capture delay keep bounded queues and predictable repeat/drop behavior.
- No NDI capture, copy, send, or transfer wait blocks the render thread.
- Receiver connect/disconnect changes output demand without affecting input readiness.

### Stage 8: screen output

**Status:** Pending

Deliverables:

- Make screen presentation a PTS-aware buffered output driven by selected-monitor refresh/presentation cadence.
- Add configurable slot depth, explicit preroll, cadence conversion, repeat/drop policy, and presentation metrics.
- Associate GL sync with shared render-slot reuse so compositor stalls cannot block the program render thread.

Exit criteria:

- Program and monitor rate mismatch behaves deterministically without consuming arbitrary newest frames.
- Window/compositor stalls remain contained by the bounded queue.
- Fullscreen monitor selection and existing context-lifetime behavior remain correct.

### Stage 9: FFmpeg sources

**Status:** Pending

Deliverables:

- Implement continuously hot, bounded demux/decode-ahead workers for playing sources.
- Select variable-frame-rate media by timestamp and support seek/loop/reopen epochs.
- Integrate hardware-decoded surfaces without forced CPU readback.

Exit criteria:

- Graph switches do not implicitly pause, seek, or restart playback.
- Decode queues remain bounded under source/program rate mismatch and skipped program evaluations.
- Seek, loop, discontinuity, and EOF behavior are deterministic.

### Independent follow-up: atomic option batches

After the timing foundation is stable, add all-or-nothing option-only batches applied at the next frame boundary. This
work may proceed independently and must not introduce target-PTS scheduling, graph mutation transactions, or candidate
graph reconstruction in its first version.

### Future extensions

- DeckLink hardware/reference and controlled input-follow clock sources.
- Configurable clock switching and discontinuity handling.
- PTS/future-frame-addressed control transactions and transactional graph mutations.
- Audio frames, resampling, mixing, and scheduled output on the same timeline.

## Validation strategy

Timing behavior needs deterministic simulation in addition to hardware testing. Add a fake clock, fake timed sources,
fake transfer delays, and fake buffered outputs. Cover at least:

- integer and 1000/1001 program rates;
- a source clock a few ppm faster and slower than program time;
- callback jitter that must not cause phase jumps;
- hysteretic drop/repeat behavior and bounded queue depth;
- 50-to-60000/1001 and 30000/1001-to-60 cadence conversion;
- small and multi-frame render overruns without accumulated drift;
- source timestamp discontinuity, reconnect, seek, and epoch rejection;
- output preroll, underrun, late completion, and recovery;
- every node preparing/completing while only the active closure submits and executes;
- every submission hook being invoked across the active closure before any execution hook begins;
- inactive inputs continuing capture, decode, queue advancement, and normal transfer production;
- switching an input into the rendered closure without device, decoder, queue, or transfer warm-up;
- continuously hot inputs remaining memory-bounded while no output consumes them;
- queue overflow and transfer memory-budget failure;
- SDK buffer-object reuse with a fresh transfer lease for every write-access cycle;
- repeated DeckLink format changes without concurrent old/new full-size allocator pools;
- cable disconnect and reconnect without a render-thread stall;
- node removal with callbacks, scheduled frames, allocator buffers, and GL transfers outstanding;
- output shutdown when the device disappears without delivering its final stopped callback;
- safe shutdown with queued callbacks, frames, and GL resources.

Hardware validation remains required for DeckLink reference clocks, scheduled playback, unplug/reconnect behavior,
NDI network jitter, CUDA/DVP/PBO transfer paths, and display-compositor timing. Long-running soak tests should verify
that recovered phase and queue depth remain bounded instead of slowly drifting. The local DeckLink output-to-input
loopback is useful for lifecycle, pacing, and long-running ownership tests, but independent source-clock tests are still
required because loopback shares unusually favorable hardware timing.

## Decisions to preserve during implementation

- Keep one stable graph snapshot for each program frame.
- Keep every input hot and advance every node through `prepare()` and `complete()` on every evaluation.
- Use render demand only to select the submission/execution closure, never to suspend an input pipeline.
- Keep native state authoritative and validate before broadcast.
- Keep normal node and GL-resource destruction on the render thread with a context current.
- Prefer bounded queues and explicit selection-time drop/repeat decisions over unbounded buffering. Once a frame is
  selected, its mandatory execute-time transfer wait must not be converted into an implicit repeat.
- Treat source timestamps as source-local observations, never automatically as program PTS.
- Preserve SDK-specific synchronization primitives behind common media timing contracts rather than forcing all
  sources through identical implementation details.
- Introduce each stage in a buildable, runnable state; do not replace every input and output in one change.
