# Frame timing and synchronization plan

## Status and purpose

This document describes the planned transition from the current fixed-rate render loop to a PTS-aware, buffered,
broadcast-oriented frame pipeline. It is a design and migration plan, not a description of behavior that has already
been implemented. The current runtime contracts remain documented in [architecture.md](architecture.md) and
[gpu-and-media.md](gpu-and-media.md).

The design has four central rules:

1. Miximus owns one explicit program timeline and one active clock source.
2. Source timestamps identify positions on source-local timelines; they are not absolute Miximus timestamps.
3. Inputs adapt their local timelines to the program timeline, while outputs buffer frames for scheduled playout.
4. Graph mutations are validated as transactions and become visible atomically at a named program frame.

## Goals

- Support rational broadcast frame rates such as 24, 25, 30, 50, 60, 24000/1001, 30000/1001, and 60000/1001.
- Keep media PTS separate from wall-clock deadlines and callback arrival times.
- Tolerate short render-time variance through bounded output buffering.
- Skip obsolete program frames without accumulating clock drift.
- Select an appropriate input frame for each program PTS instead of consuming the most recently completed frame.
- Adapt independent input clocks using stable clock recovery, hysteresis, and deliberate repeat/drop decisions.
- Allow callback-driven inputs to transfer arriving media continuously while active, then select only the frame needed
  by the current program frame. Pull-driven inputs may start work during preparation, but neither model should perform
  avoidable work for inactive graph branches.
- Keep decode, blocking SDK work, CPU image processing, and host/GPU transfer waits off the render thread.
- Support DeckLink, NDI, screen presentation, and future FFmpeg sources through common timing contracts while
  preserving their different native memory and clock models.
- Allow external controllers to submit a batch of graph changes that takes effect on exactly one frame boundary.
- Expose enough timing and queue metrics to diagnose late frames, drift, underruns, and sustained overload.

## Non-goals for the first migration

- Rendering one graph independently at several unrelated program frame rates.
- Interlaced video; the program timeline and all supported inputs and outputs are progressive-only.
- Seamless switching between clock sources without flushing and re-prerolling affected pipelines.
- Turning the graph into a generally parallel task executor.
- Blocking the render thread indefinitely to preserve every input or output frame.
- Treating callback arrival time as a precise media clock.

The initial model should have one configured program format. Outputs at another cadence must use an explicit
rate-adaptation policy. Supporting several independent program timelines later would require separate evaluation
caches and potentially multiple graph evaluations for the same logical scene.

## Current limitations

The current loop stores a steady-clock `timestamp`, a media `pts`, and a hardcoded 1/60-second duration. When it is
late it advances the deadline separately from the PTS, so the two timelines can diverge. Deadlines are not managed by
a configurable clock object, and lateness has no policy beyond a single-frame adjustment.

The current graph frame performs these operations:

1. update the stable render snapshot;
2. call `prepare()` on every node;
3. lazily execute dependencies of every `must_run` root;
4. call `glFinish()` globally;
5. call `complete()` on every node.

This preserves a stable graph and render-thread GL destruction, both of which should remain. It does not establish an
active subgraph before preparation, so an input cannot both begin work early and avoid all work while unused.
`prepare()` also currently combines configuration maintenance, device lifecycle, status updates, allocation, and
per-frame acquisition.

The transfer services use monotonic versions or arbitrary integer tags and publish the latest completed transfer.
Those are useful implementation details but do not identify the media frame for which a transfer was requested.
Texture and framebuffer interfaces similarly expose raw pointers without PTS, epoch, or readiness metadata.

## Program timeline

### Program format

The application should own a program-format value containing at least:

- rational frame-rate numerator and denominator;
- resolution;
- nominal frame duration in `utils::flicks`;
- target render-ahead/output-buffer latency;
- selected clock source and late-frame policy.

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
    steady_time playout_time;

    bool discontinuity;
};
```

The concrete steady-clock type and placement will be decided during implementation. The important distinction is:

- `pts` identifies media on the program timeline;
- `target_time` maps that PTS to the active scheduling clock;
- `render_deadline` is the latest useful completion time;
- `playout_time` includes the configured output buffering;
- `frame_number` names a graph/configuration boundary;
- `epoch` invalidates timestamp relationships after a restart, seek, format change, or clock discontinuity.

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

When a program frame is skipped, its frame number, PTS, scheduled graph transactions, and output accounting must all
advance consistently. A transaction assigned to a skipped render frame still needs defined application semantics;
the recommended rule is to apply it at the boundary where that frame is skipped so all later frames observe it.

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
- a frame that could not become transfer-ready before the deadline.

## Clock sources

The frame scheduler should depend on a clock-source interface rather than sleeping directly in `main.cpp`.

Initial clock modes should be introduced in this order:

1. internal `steady_clock` scheduling;
2. DeckLink output hardware/reference clock;
3. optional DeckLink input-follow mode.

There is exactly one authoritative program clock. Normal inputs adapt to it independently. If an input is explicitly
selected as the master, its callback must publish clock observations to the scheduler through a bounded/thread-safe
bridge; SDK callbacks must never invoke graph evaluation directly.

NDI FrameSync is designed to adapt NDI inputs to the receiver's clock and should normally follow the program timeline,
not become its hard clock source.

Changing the active clock or program format creates a new timeline epoch. The first implementation may flush input
selection state, flush/restart output scheduling, and preroll again rather than attempt a seamless clock transition.

## Node lifecycle and active graph

### Separate maintenance from frame work

The responsibilities currently in `prepare()` should be divided conceptually:

- configuration/maintenance: observe changed options, schedule device reconfiguration, publish status, and process
  asynchronous lifecycle results;
- frame preparation: select/reserve input media and initiate or associate asynchronous work with one frame context;
- execution: resolve dependencies and submit CPU/GPU transformations for the prepared frame;
- retirement: release per-frame reservations and publish completed work without blocking.

Configuration and device lifecycle work should be event-driven where possible. Blocking SDK calls must run on device
control workers rather than in a per-frame render callback.

### Discover demand before preparation

The manager should first determine which sinks need a frame. Examples include:

- enabled DeckLink outputs;
- NDI outputs with active receivers;
- enabled screen outputs;
- future encoders, recorders, and explicit preview consumers.

It can traverse connections upstream without executing nodes and produce the active subgraph. Only nodes in this
closure receive frame preparation and execution. Inactive sources may retain lightweight device reception where an SDK
requires it, but should stop decode, conversion, transfer, and transient allocation.

This retains once-per-frame execution while allowing all active inputs to start transfers before downstream execution.
It also replaces the current `must_run` boolean with a more explicit sink-demand contract.

### Prepared frame tickets

An input's frame preparation should park a ticket rather than expose a partially ready texture. The ticket should
carry the media identity and a readiness state such as:

- ready;
- transfer submitted with a shared GPU fence;
- reserved but not submitted;
- missing;
- discontinued.

Preparation does not necessarily initiate the transfer. Callback-driven devices such as DeckLink must continuously
accept frames into a bounded transfer stream while capture is active; by the time a program frame is prepared, an
appropriate source frame may already be submitted or ready. Preparation selects and reserves that ticket. Pull-driven
sources such as NDI FrameSync or a decoder may instead use preparation to request the desired frame. Both paths expose
the same ticket contract to execution and retain its storage until frame retirement.

Execution may enqueue a GPU-side wait when a shared-context transfer has already been issued. It must not perform an
unbounded CPU wait. If the worker has not submitted the transfer before the remaining render budget expires, the node
uses its declared fallback policy: repeat, black/missing, or mark the program frame unusable for a mandatory source.

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
possible. Capture and upload submission happen asynchronously as frames arrive. Active preparation selects and parks
the appropriate already-ready or in-flight upload ticket for the requested program PTS; execution resolves that ticket
without asking the device to capture or re-upload the frame.

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

The scheduler should enqueue a pull request containing the desired program frame identity. The capture worker calls
`NDIlib_framesync_capture_video()`, copies into an upload lease, and tags the result with the requested program PTS.
The returned NDI timecode, receive timestamp, format, and frame rate should be retained for diagnostics and
discontinuity detection even though FrameSync performs the primary time-base correction.

Calling FrameSync from a worker is appropriate because host memory copying should stay off the render thread. The
request queue must be bounded and obsolete requests must be dropped deliberately.

### FFmpeg input

File and stream nodes should use dedicated or managed demux/decode workers with bounded decode-ahead queues. They
should retain packet PTS/DTS, `AVFrame::best_effort_timestamp`, duration, and stream time base, then rescale into
`utils::flicks`.

The source epoch changes on seek, loop, reopen, and discontinuity. Variable-frame-rate media must be selected by
timestamp rather than an assumed frame rate. When a graph branch is inactive, decoding should stop after a small
retained window; reactivation may resume or seek according to the node's explicit playback semantics.

Hardware-decoded surfaces should enter the same prepared-ticket and transfer contracts without forcing CPU readback.

## Output scheduling and buffering

Outputs consume frames by intended presentation PTS rather than simply taking the latest completed transfer. Every
output queue must be bounded and define overflow, underrun, repeat, and shutdown behavior.

### DeckLink output

Preserve scheduled playback and callback replenishment, but associate each rendered/downloaded frame with its intended
DeckLink schedule time. Maintain explicit low, target, and high queue watermarks. On underrun, repeat the previous frame
or schedule black according to policy; discard a rendered frame that arrives after its useful scheduling point.

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

An NDI output with no receivers should stop demanding upstream frame work while continuing lightweight connection
monitoring.

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

## Atomic graph transactions

The render snapshot already gives Miximus a frame boundary, but individual WebSocket commands are currently
validated, applied, and broadcast independently. Add a transaction command containing:

- transaction ID;
- optional expected graph revision for optimistic concurrency;
- an ordered list of node and connection operations;
- optional target frame number or program PTS;
- an explicit policy or error for a target that is already in the past.

The configuration side should apply the operations to a candidate graph state, validate the complete result, and
either reject everything or enqueue one immutable commit. Validation includes options, interfaces, directions,
connection limits, displaced connections, and cycle detection.

At the assigned frame boundary, the scheduler applies the transaction to the authoritative/render state, increments
the graph revision, and broadcasts one applied event containing the effective frame, epoch, and revision. Protocol
responses should distinguish:

- rejected;
- accepted and scheduled for frame N;
- applied at frame N.

The server should expose current epoch, frame number, PTS, program rate, graph revision, and an estimate of the next
frame boundary so external controllers can schedule changes. Commands without an explicit target may default to the
next safe boundary.

## Threading and ownership

The intended roles are:

- scheduler/render thread: frame boundaries, active graph evaluation, root GL submission, transaction application;
- configuration thread: parsing, validation, and transaction construction;
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
- skipped frames and sustained-overload state;
- pending/applied graph revision and transaction frame.

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

### Stage 1: timing primitives and measurement

- Introduce rational program format, frame context, frame number, epoch, and scheduler metrics.
- Preserve the existing node lifecycle while passing the new context through `app_state_s`.
- Add deterministic clock/unit-test support before changing hardware behavior.

### Stage 2: internal frame scheduler

- Replace the loop-local 60 Hz timing in `main.cpp`.
- Implement anchor-based deadlines, configurable rate, bounded lateness, and multi-frame skipping.
- Keep internal `steady_clock` as the only clock source initially.

### Stage 3: graph transactions

- Add graph revisioning and atomic candidate validation.
- Add batched WebSocket messages and scheduled frame-boundary application.
- Preserve existing single-operation messages as transactions of one operation during migration.

### Stage 4: demand and lifecycle split

- Determine active sinks and traverse the upstream closure before frame preparation.
- Separate maintenance/configuration reactions from active per-frame preparation.
- Replace `must_run` and remove or redefine the unused `wait_for_sync` trait.

### Stage 5: timed media and transfer tickets

- Add media frame identity, source epoch, and readiness to upload/download boundaries.
- Add bounded selection APIs instead of relying on `consume_latest()`.
- Keep source timing and readiness inside input synchronizers and transfer tickets; graph texture/framebuffer values
  represent the current program frame without exposing those details downstream.

### Stage 6: first scheduled output

- Make DeckLink output fully PTS-aware with explicit preroll and watermarks.
- Establish output deadline, repeat, late, and underrun semantics.
- Preserve the existing zero-copy download-lease-backed scheduled frames and asynchronous stop ownership while adding
  timing selection; do not rebuild device lifecycle management into the frame scheduler.
- Validate behavior using both fake output tests and hardware metrics.

### Stage 7: synchronized live inputs

- Migrate DeckLink capture to source-clock recovery and program-PTS selection.
- Extend the existing callback-submitted upload stream with bounded timed-frame selection. Do not move capture or
  transfer initiation into `prepare()` and do not assume SDK buffer allocation/release occurs once per captured frame.
- Migrate NDI FrameSync requests to program-frame tickets.
- Exercise independent-rate, jitter, disconnect, and reconnect behavior.

### Stage 8: remaining outputs

- Make NDI sending PTS-selective and cadence driven.
- Make screen presentation a PTS-aware buffered output driven by the selected monitor's refresh/presentation cadence,
  with explicit preroll, watermarks, repeat, drop, and underrun behavior.

### Stage 9: FFmpeg sources

- Implement timestamp-driven bounded decode and selection.
- Add seek/loop epochs, inactive-demand behavior, and hardware-frame integration.

### Stage 10: external clock modes and audio

- Add DeckLink hardware-reference clock mode and controlled input-follow mode.
- Add clock switching/discontinuity behavior.
- Introduce audio frame, resampling, mixing, and output scheduling on the same timeline.

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
- a graph transaction applied on exactly its assigned frame;
- transaction behavior when its render frame is skipped;
- inactive branches producing no decode/transfer work;
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
- Keep native state authoritative and validate before broadcast.
- Keep normal node and GL-resource destruction on the render thread with a context current.
- Prefer bounded queues and an explicit dropped/repeated frame over render-thread stalls.
- Treat source timestamps as source-local observations, never automatically as program PTS.
- Preserve SDK-specific synchronization primitives behind common media timing contracts rather than forcing all
  sources through identical implementation details.
- Introduce each stage in a buildable, runnable state; do not replace every input and output in one change.
