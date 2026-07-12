# Render-thread blocking audit

This audit records operations that can stall Miximus's 60 Hz render thread. It distinguishes deliberate GPU ordering from
unbounded CPU, filesystem, SDK, network, and thread waits. Revisit it when node lifecycle or media integrations change.

## Rules

- Discovery, filesystem access, network I/O, CPU-heavy rendering, and potentially blocking SDK setup belong on the
  application thread pool or a dedicated owner thread.
- Render-thread code may poll completed work and publish or consume ready results. It must not wait for background work.
- OpenGL work stays on a thread with the appropriate context current. Moving a call off the render thread requires an
  explicit shared context and lifetime protocol.
- Node destruction and application shutdown may join owned workers, but ordinary `prepare`, `execute`, and `complete`
  calls must not.
- GPU-side ordering such as `glWaitSync` is not automatically a CPU stall. CPU fence waits must only occur after known
  completion or on a worker.

## Registry results

| Area | Result |
| --- | --- |
| Font registry | Fontconfig/Win32 enumeration and logging build a replacement map without the mutex. The completed map is swapped under the write lock; destruction of the previous map happens after unlocking. |
| NDI registry | Discovery already runs on its own thread. Source copying, change comparison, and logging happen off-lock; only replacement of the published list is locked. |
| DeckLink registry | SDK interface and device-name queries happen before locking. Arrival/removal only mutate maps under the lock. Device removal tracking is populated on arrival. |

Registry getters still copy published names under a shared lock. These copies are bounded in-memory operations, not
external blocking calls. If registry lists become very large, immutable shared snapshots would remove this remaining
reader critical section.

## Render-path findings

### Fixed

- Teleprompter line rendering now polls worker futures before acquiring their mutexes. A busy line is skipped for the
  frame instead of stalling rendering.
- Teleprompter reconfiguration and render-line shrinking no longer call `future::get()` until the future reports ready.
- NDI output readback completion occurs after the frame-wide GL finish, and network transmission remains on its worker.
- Screen output drops work when no safe buffer is available instead of waiting for the display thread.

### High-priority follow-up

#### Text node rasterization

`src/nodes/text/text.cpp` calls `render_text()` from `prepare()`. That path can load a font file, shape and rasterize the
entire string, allocate a CPU surface, copy its pixels, and submit a texture upload. This is the clearest remaining
per-frame stall risk. It should follow the Teleprompter model: build a generation-tagged CPU result on the thread pool,
then publish only the newest completed result. Any GL upload must use a shared context or be kept as a short render-thread
publication step.

#### DeckLink input/output reconfiguration

DeckLink enable, disable, stream start/stop, mode enumeration, and scheduled-playback restart currently occur in node
`prepare()` or render-thread destruction. SDK calls can block on drivers or hardware. Moving them requires a dedicated
device-control thread per node (or registry-managed control executor), with immutable requested configuration and an
atomically published ready/error state. GL transfer resources must still be created and destroyed with a valid context.

#### NDI receiver/sender lifecycle

NDI receiver and sender creation/destruction, plus worker joins, currently occur from `prepare()` when options change.
Frame capture uses the frame-sync API without a wait timeout and sending is already delegated, but SDK lifecycle calls
can still stall. A dedicated owner thread should process desired source/name changes and publish ready handles. Care is
required because the render thread currently consumes those handles directly.

### Medium-priority follow-up

- Screen-output disable and destruction call `join()` on the display thread. The worker is signalled first, but a driver
  stall in buffer swap or context teardown can still delay the render thread. Consider asynchronous retirement with GL
  resources destroyed by the display thread before it reports completion.
- Registry-backed status lists are converted to JSON and compared under the status-registry mutex. Version checks avoid
  doing this every frame, but large application-wide catalogues should eventually be published once as shared system
  state rather than duplicated per node.
- Shader, framebuffer, texture, and transfer allocation can cause driver work on first use or format changes. These are
  not ordinary CPU blocking calls and often require the render context, but caches and preallocation can reduce frame
  spikes where latency matters.

## Shutdown-only waits

The NDI discovery join, DeckLink uninstall timeout, node worker joins, and web-server stop wait occur during ordered
shutdown. They do not affect steady-state frame timing. Keep them bounded where third-party SDKs permit it, but do not
replace joins with detached threads that can outlive their owners.
