# GPU and media

## Device, recordings, and windows

`core::app_state_s` owns one Vulkan 1.3 device and the upload/readback services. `gpu::window_s` is now only a GLFW
window and monitor service. Windows use `GLFW_NO_API`; Linux uses X11/XWayland to preserve saved desktop positions and
pixel sizes. Rendering and presentation still use Vulkan. Window creation, destruction, monitor queries, and event
polling stay on the main/render thread. Presenter workers read cached drawable dimensions.

Device selection requires the conversion storage-image features as well as the render format floor. Presentation
additionally requires swapchain maintenance; each screen surface must support opaque composition, the selected
sRGB format, and transfer-destination usage. Unsupported configurations are rejected with diagnostics rather than
selecting a different rendering path. An unavailable fullscreen monitor fails that screen output instead of opening
a window on another target.

Screen-output failures are latched in `screen_error` with `connected=false`. They stop demanding graph execution;
changing the enabled state or window/monitor configuration explicitly clears the failure. Resize and monitor
refresh-rate notifications do not retry a failed endpoint. GPU device loss, submission/completion-query failure,
and poisoned CUDA contexts terminate the process with a stderr diagnostic and failure exit code, without waiting
for unsafe resource retirement. Pending CUDA transfers and synchronous upload waits have a 30-second fatal
liveness limit, independent of frame deadlines. Normal short `cudaErrorNotReady` periods remain expected.
Initialization has a 60-second watchdog; exceptional graph teardown uses the normal shutdown watchdog.

Run `python3 scripts/test_screen_output_failure.py [build/miximus]` in a display session to verify unavailable
monitor status, failure retention, explicit reconfiguration, and disabling the output. The script uses temporary
settings and refuses to run alongside another application instance on the local API port.

Ordinary nodes use `gpu::drawing.hpp` and `app->commands()` for typed operations. The sole GPU implementation lives
directly in `src/gpu/`, with Vulkan/VMA state and pipeline internals in `src/gpu/detail/`. Public resource headers
contain no native Vulkan types; dependency integration remains in `src/wrapper/vulkan/`. Internal device ownership
separates pipeline resources, submission scheduling and deferred retirement into concrete components under
`src/gpu/detail/`; resource and recording state have their own headers. No OpenGL context or bridge remains. Volk and VMA are pinned submodules; Vulkan headers come from the installed SDK.

The graph, upload service, readback service, and each presenter own independent `recording_context_s` instances.
Each context has its own command pools and a bounded set of in-flight recordings. A producer holding an unfinished
recording cannot reserve the graph's capacity. `try_record()` returns immediately if that context is full; transfer
workers retry, while the graph drops an unfinished evaluation and increments `gpu_recording_drops`. Descriptor pools
grow in reusable pages with the workload, so page size is not a draw limit. Completed arenas retain their pages for
reuse; descriptor allocation failure remains a recoverable frame drop.

Recorders build command bodies using local image layouts. A submission worker resolves the initial image transitions
against the actual submission order and records a small prologue. It alone submits to the graphics queue and commits
layouts and resource timeline values after success. Context-local submission order is preserved; contexts are serviced
in turns. Graphics and transfers currently share this GPU queue, but they record independently and the render thread
never takes the queue/presentation mutex. A separate presentation queue is used where available. This follows Vulkan's
[explicit execution and memory dependencies](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

`recording_s::submit()` enqueues a recording and returns a pending completion ticket; queue acceptance and GPU
completion are separate states. Recordings, command pools, descriptors, and referenced resources stay alive through
actual completion. Abandoning an unsubmitted recording releases its tentative uses without creating an unsignalled
dependency. Native allocations retire after their actual last submitted use. `recording_s::wait_for()` records a typed
GPU dependency. The submission worker defers that context until the producer has a native timeline value, so a wait
for a future submission cannot deadlock the shared graphics queue; other contexts can continue.

A frame scope owns pending output leases with RAII. Once demanding-node execution succeeds, `commit_gpu_frame()` queues
the final recording with publication callbacks. Only successful native submission publishes those leases, on the
submission worker. Earlier command batches can be flushed during execution without publishing unfinished outputs.
Aborting the evaluation releases unpublished leases; already queued commands still retain their resources.
`complete()` remains the existing CPU lifecycle/cleanup hook and does not imply GPU completion.

NDI and DeckLink capture enqueue each upload **before** publishing the frame into the timed input FIFO, independently
of graph demand. The render thread selects by PTS and waits for that exact upload during consumption, after the
configured buffering interval (currently one frame for NDI and three for DeckLink). It waits only until the exact upload
has a graphics-consumable submission ticket, then records `commands().wait_for(frame->upload_completion())`. GPU
completion is a GPU dependency, not an additional CPU wait. Selection never filters frames by readiness.
Pending graph commands may be submitted before consumption so their GPU work can overlap the selected input wait.

## Textures, drawing, and color

`gpu::texture_s` directly retains a Vulkan image allocation and its recording/timeline use. `gpu::buffer_s` directly
retains a Vulkan buffer allocation. Format, sampling, and channel order have one shared definition; there is no second
image wrapper or backend adapter. Ordinary working targets remain four-channel 16-bit **UNORM**, preserving the existing
bounded linear-light contract. Raw RGBA/BGRA/BGRX/ARGB transfers use RGBA8 storage plus explicit shader component
mapping. Packed v210 occupies device buffers with SDK row-stride padding, not filtered images.

There is no framebuffer resource class or Vulkan framebuffer object: dynamic rendering targets textures directly. The
graph keeps its existing `texture` and `framebuffer` protocol names, represented by `const gpu::texture_s*` and
`gpu::texture_s*` respectively. This preserves read-only fan-out and ordered mutable-target connections without a GPU
wrapper. Named drawing options and typed draw/mix parameters describe normalized node geometry, pixel viewport/scissor,
opacity, premultiplied blending, transfer functions, and component order. Packing/unpacking and color matrices live in the GPU layer.
C++/shader matrix rows are explicitly padded; the legacy color conventions are retained. Text and teleprompter surfaces
still render on the CPU.

`blend_mode_e` selects video-space or linear-light A/B interpolation. `compositing_e` separately selects replacement or
premultiplied source-over on the destination. Keep these enums through the drawing and recording APIs; encode
shader/native values at the implementation boundary. Apply the same distinction to color-conversion, transfer-direction
and ownership choices.

Uploads configured for mipmaps generate them before publication. NDI/DeckLink conversion and framebuffer-to-texture
boundaries also generate mipmaps, preserving the OpenGL placement of this work. A minifying consumer defensively
regenerates any remaining dirty chain; one-to-one and magnifying draws use the base-level view. Base writes invalidate the chain, including across program frames. Integer packing buffers have no mip
chain. The renderer uses top-to-bottom image coordinates consistently.

GLSL sources and shared includes live under `shaders/`, outside the runtime resources directory. Only their compiled
binaries are bundled. GLSL is compiled to validated SPIR-V by the pinned glslang toolchain and bundled into
`static_files` by the existing file bundler. All finite pipelines are warmed during device startup, before frame
processing. The app does not maintain a pipeline cache on disk; see the
[implementation status](vulkan-progress.md) for the decision.

## Completion and bounded transfer ownership

The [DeckLink direct-memory contract](decklink-direct-memory.md) remains mandatory even while DVP itself is absent. The
private `transfer_backend_i` interface owns allocation, registration, GPU hand-offs and completion; SDK-facing leases
expose that backend's stable address. A future DVP backend must not introduce an intermediate CPU frame copy or require
changes to DeckLink's lease ownership. CUDA remains supported independently of comparative results on any single GPU.

`completion_s` retains a submission ticket that receives a queue timeline value only after native submission. Readiness
polling reads worker-published completion state without calling the driver. Explicit waits are finite and stop-aware. Images and buffers also track tentative recording uses, so an abandoned output target cannot be reused
while a recorder still references it.

The shared transfer services retain their existing bounded per-stream pools, memory budgets, exact upload IDs, and owned
leases. Each service has a resource worker for allocation, registration and destruction, separate from its transfer
progress worker. Starting or resizing a stream cannot delay reporting unrelated transfers simply because pool
allocation is still running. Conversion targets for NDI/DeckLink inputs and DeckLink output are allocated alongside
their transfer pools, with the same UNORM16 precision and mipmap policy, and are included in the memory budget.
Eight-bit transfers use raw RGBA8 images; v210 uses raw-word buffers and compute conversion. On Linux, builds with the CUDA toolkit
use Vulkan staging by default. `--use-cuda` requests direct CUDA transfers when the selected Vulkan device
has a matching, usable CUDA device and external-memory/semaphore support. Startup also creates and imports every
required transfer representation, sampling mode and direction through the production allocation/registration path.
If any qualification fails, CUDA is disabled for the entire run before streams start; all streams use Vulkan staging.
Without `--use-cuda`, startup skips CUDA probing entirely. Selection is fixed for the device lifetime; CUDA transfer or
format failures never trigger a per-stream fallback. Backend controls and verification are documented in
[cuda-transfers.md](cuda-transfers.md).

Vulkan staging allocates and maps host memory through VMA with the requested alignment. The CUDA backend instead uses
aligned CUDA-pinned host storage and imports the frame's actual dedicated exportable RGBA8 image or raw-word buffer,
with external semaphores and explicit queue-family ownership hand-offs. CUDA copies directly between host memory
and that resource; no intermediate device frame or Vulkan transfer copy is involved. Channel swizzling remains in
shaders so all byte orders share the same CUDA-compatible format. CUDA mode never falls back by format. Full-overwrite inputs prefer sequential host writes; read/modify/write inputs and SDK
output allocations use cached host-accessible memory. Noncoherent Vulkan host writes are flushed before GPU use, and
readback memory is invalidated only after completion. DeckLink may request writable access even to output buffers, so
output allocations permit it while retaining the external lease.

An upload lease owns its host address for one producer write cycle. Submission queues the exact ID. Timed FIFO frames retain their submitted upload lease until consumption or eviction;
selecting one upload preserves other FIFO-owned uploads. Eviction discards an unconsumed upload through its
lease, and reclamation still waits for GPU/SDK use. The worker publishes the upload dependency as soon as graphics consumption can be queued, then continues polling
actual completion. Completed-only selection remains available to CPU-driven callers. A selected frame remains current
until replacement. The
slot cannot return to the producer while a CPU frame holder or any GPU recording/submission still uses it. Timed sources
wait for their exact selected ID and never substitute an older ready upload.

A readback target owns a render slot and exposes either its texture or its packed buffer. Packed v210 frames expose
their buffer and host row layout, never a fabricated texture. After drawing, `submit(completion)` queues it with its
actual render dependency. The worker queues the copy with a GPU wait for rendering and publishes readable frames in
submission order, only after copy completion and invalidation. Each polling pass retries the whole pending set before
new arrivals and waits once per pass, rather than once per operation. A returned readback lease reserves its host memory
until every SDK/worker reader finishes. Slot states and transfer failures remain visible in existing node metrics. A failed transfer stays quarantined until
its external leases and GPU uses retire; the worker then destroys its backend allocation and schedules a replacement.
Transfer failure does not permanently consume pool capacity or masquerade as an allocation failure. Device loss still
requires device recovery; replacing a slot cannot repair a lost device.

Stream destruction is queued on the service worker. Application shutdown releases graph nodes, drains SDK control
workers and external references, then drains transfer services, retires GPU resources/device, and finally terminates
GLFW. CPU mutexes or `complete()` never substitute for GPU completion.

## Screen presentation

Screen output keeps bounded program slots and its PTS-aware timed queue. The Vulkan WSI worker invokes the frame
source after acquiring a swapchain image, outside GPU recording. The callback collects submitted program frames,
applies preroll, and chooses the nearest PTS across both the retained frame and queued frames. Ties retain the older
frame. Obsolete frames return to the free list through their leases; future frames remain queued. The presenter passes
the selected producer's timeline semaphore into its GPU submission. It does not wait for producer GPU completion on
the CPU. The slot remains protected through the consumer copy by its lease and the texture's last-use timeline.

Screen swapchains use FIFO presentation. When `VK_KHR_present_wait` and `VK_KHR_present_id` are supported, the worker
waits for the submitted presentation ID before preparing the next display interval. The copy is submitted ahead of
that interval, rather than sleeping until the intended display time before beginning the copy. A display-specific
clock filter estimates phase and refresh period from these notifications; it accounts for missed refreshes and delayed
host notifications without accumulating extra intervals. The nominal GLFW refresh is only the initial period estimate.

Present-wait does not expose an exact scanout timestamp. The status therefore reports **Display completion estimate**;
its observations include host wake-up error. The current NVIDIA machine supports present-wait but does not expose
`VK_GOOGLE_display_timing`. Devices without present-wait use a nominal schedule with submission lead time and FIFO
backpressure, explicitly labelled **Nominal FIFO estimate**. A submission return or maintenance fence is never treated
as an actual display timestamp. See the [present-wait timing contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForPresentKHR.html).

The initial presentation establishes the buffered program delay. Every display notification, including a repeated
source frame, contributes to the rolling average of actual completion minus predicted completion. The phase correction is slewed to keep notification jitter from toggling cadence decisions near a source-frame
boundary. It corrects the next display prediction before mapping it to program time. Source-PTS rounding for frame-rate conversion is excluded
from that correction: otherwise deliberate repeats/skips become accumulating latency. The actual completion-minus-PTS
average remains the reported output latency. The original full-queue/oldest-frame fallback remains separate from
continuous display feedback and realigns a target that has fallen entirely behind the retained program frames.

There is one pacing worker and no intermediate mailbox on the screen path. Acquisition waits at most one millisecond
per attempt, and cadence/dependency waits observe worker cancellation. None of these waits holds a graph recorder.
Slot acquisition uses a short metadata lock; temporary lock contention does not count as exhausted output capacity.
The worker retires GPU/WSI uses before reporting stopped, so the node's asynchronous replacement path does not move
retirement waits onto the render thread.

The separate publication API exercised by window tests remains latest-wins and submits each publication once, redrawing retained
content only for swapchain changes. `presentation_drops` exposes mailbox replacements independently of timed selection
drops. The screen's callback path does not publish through this mailbox.

Swapchains use sRGB attachments; the final blit performs display encoding exactly once. Acquisition semaphores
retire after their GPU wait; per-image present semaphores retire using swapchain-maintenance presentation fences. A
separate presentation queue is used when available. Queue-present return and GPU completion are never reported as
physical scanout observations. Swapchain maintenance is currently required for bounded, correct presentation retirement.

Linux monitor IDs use Xrandr connector names, preserving saved display selections. Compositor-specific ICC/color
metadata and physical scanout recovery remain unavailable. Resize/DPI changes cause bounded presenter recreation.
Surface loss stops the failed presenter so the main thread can recreate its window and surface.

## DeckLink

DeckLink input uses an `IDeckLinkVideoBufferAllocatorProvider` and `IDeckLinkVideoBufferAllocator` implementation. SDK
capture buffers hold upload leases, allowing DeckLink DMA to write directly into backend-owned host memory. The SDK
callback records the source PTS and frame metadata, detaches the write cycle's one-shot upload lease from the reusable
custom buffer, enqueues its upload immediately, and stores that lease in a bounded timed-source queue. The callback
performs no native GPU recording or completion wait. The render node selects the frame assigned to the current program
PTS; execution waits for and consumes that same upload ID before unpacking its v210 buffer. DeckLink may finish buffers in a different order from
their `StartAccess()` calls, so upload IDs identify transfer transactions rather than media order.

DeckLink may retain and reuse allocator buffer objects across captured frames rather than requesting a new object for
every frame. Each buffer therefore acquires a fresh one-shot upload lease from `StartAccess(bmdBufferAccessWrite)`.
`GetBytes()` exposes that lease's host address for the current SDK write cycle, and the capture callback submits it once that write cycle is complete.
Keeping one lease for the full lifetime of a DeckLink buffer would upload only its first frame and exhaust the transfer
slots after the initial pool. Completed GPU textures remain owned by the upload stream independently of the DeckLink
buffer object.

The custom allocator exposes at most eight reusable DeckLink buffer objects and reports `E_OUTOFMEMORY` when DeckLink
has all eight checked out; this is how the SDK establishes the bounded capture pool. Its upload stream preallocates
eight transfer slots and may grow to sixteen: four may be retained by timed selection, while the remainder cover the
published texture, DeckLink DMA writes, uploads, and asynchronous reclaim without coupling capture cadence to the
render thread. These limits are intentionally separate because DeckLink buffer-object reuse and transfer-slot lifetime
are independent. Every address still comes from a transfer backend; there is no node-owned staging allocation.

The upload stream retains its current texture until the render thread consumes a newer submitted upload;
publishing the replacement returns the former slot to the stream. The timing queue deliberately chooses whether a
program frame uses a new capture, repeats its committed capture, or has no capture before submission. Once a new frame
is selected, execution waits for it rather than silently falling back to an older ready upload. The converted framebuffer
remains the node's output only when timing policy did not select a replacement. There is no fallback frame copy. A format
change releases the old upload stream before allocating the new one, but the converted framebuffer remains visible
until a frame in the new format replaces it.

DeckLink wraps application-provided buffers in its input-frame objects. Upload-backed buffers therefore expose a private
IID, matching the SDK custom-allocator examples, so the callback can recover the original custom buffer and move its
current write lease into the timed frame ticket.
The registry owns a serialized DeckLink input-control worker. Capture start, stop, disable, device removal, and
allocator retirement all run there; the render thread never calls those potentially blocking SDK methods. A format
callback only records the pending mode and asks the render node to release its current texture. The next render prepare
acknowledges that release without waiting, after which the control worker stops capture and waits for SDK buffer
references to drain. Only then is the old allocator and upload stream retired and capture enabled at the pending mode.
Old and new full-size allocator pools never coexist.

Input-node destruction is likewise non-blocking: it clears render-owned textures, requests asynchronous capture stop,
and releases its callback reference. Control tasks retain the callback and device until the SDK callback is unregistered,
all capture buffers are returned, and transfer-stream destruction has been queued on the upload worker. Application
shutdown drains the DeckLink input-control worker before destroying the shared transfer services.

DeckLink output normally renders packed 10-bit YUV into a readback target carrying the frame's absolute program target
time in `utils::flicks`. Internal and external keyer modes instead preserve alpha through an RGBA16 intermediate,
apply the Rec.709 transfer function without changing the premultiplied-alpha representation, and
read back DeckLink 8-bit ARGB bytes. The keyed path uses ARGB because Duo 2 keyed HD p60 scheduling accepts it where
the driver's otherwise supported BGRA path can reject the first scheduled frame. ARGB byte order is described by the
shared texture-transfer format and the final shader component mapping.
The transfer worker completes readback, and ready leases drain in FIFO order into a bounded timed-output queue. That
queue releases superseded or overflowed frames according to its explicit selection policy. Before playback starts, short
non-blocking control tasks collect actual program readbacks. Once the configured buffer target is available, those
program frames are scheduled as the SDK preroll and playback begins. One additional completed program frame remains in
the timed queue so bursty SDK completion callbacks do not make selection alternate between starvation and dropping a
newly completed batch. The completion callback then compares the retained frame and queued frames to select the
closest program PTS, mapping the next hardware presentation time into the absolute program clock. It explicitly
retains frames for repeats and accounts for superseded frames as timing drops. Both ordinary and keyed output use
`CreateVideoFrameWithBuffer` to wrap the transfer lease without a copy, so the SDK frame keeps host memory reserved
until DeckLink releases it.

The single global DeckLink-output buffer target defaults to four frames and is adjustable from one to eight. The SDK's
reported minimum preroll raises the effective target when necessary. All DeckLink output nodes use the same
frame-boundary snapshot. Changing the setting uses the normal asynchronous output restart and recreates each affected
bounded readback stream; a brief output interruption is expected. Output streams enqueue their complete bounded slot
set at creation so lazy pool growth cannot cause program-frame repeats after preroll begins. Neither the callback nor
the preroll control task records GPU commands or waits for a transfer.

DeckLink registry discovery is asynchronous and protected by a shared mutex. Device arrival/removal increments `device_list_version_`. Nodes compare that version before rebuilding device-name status lists.

The registry also owns one status monitor per physical device. DeckLink `bmdStatusChanged` notifications update an
owned plain-data snapshot, while a single registry worker polls non-notifiable hardware statistics such as temperature.
Nodes copy snapshots by device name and publish them only when the snapshot version changes. Stream-specific queue
depth and frame outcome counters remain on the input/output callbacks and are throttled to status once per second.
No status or statistics SDK query runs on the render thread.

All application-provided DeckLink callbacks and buffers are complete `IUnknown` implementations: `QueryInterface` must return only matching interfaces, reference counts are atomic, and exceptions must be caught before crossing an SDK callback boundary. Input shutdown stops capture and unregisters the callback before releasing the node's references. Output shutdown calls `StopScheduledPlayback` and retains the callback and device until `ScheduledPlaybackHasStopped`; normal render ticks poll this state instead of waiting on the render thread. A bounded timeout handles devices that disappear without delivering the final callback.

SDK 16 buffer access uses `IDeckLinkVideoBuffer`: query it, call `StartAccess`, retrieve bytes, and call `EndAccess`. Linux SDK `REFIID` values do not provide normal C++ equality; follow the existing `QueryInterface` comparisons and COM pointer wrapper.

## NDI

The NDI registry runs a discovery thread, owns copied source names, protects them with a shared mutex, and increments
`source_list_version_` after changes. A separate serialized control executor creates and destroys receivers, senders,
and their workers outside the render thread. Application shutdown drains that executor before the shared transfer
services and process-wide NDI runtime are destroyed.

NDI input owns a dedicated capture thread which continuously drains `NDIlib_recv_capture_v3()`. It copies each decoded
frame into a bounded upload lease, immediately frees the SDK frame, starts the upload, and pushes source timestamp, sequence,
duration, arrival observation, and NDI timing metadata through the same `media::timed_source_queue_s<T>` used by
DeckLink. The queue's shared media-to-program clock mapping converts the arbitrary sender timestamp origin into program
time.
All-node preparation advances that queue. Active graph submission selects an already-started upload, and execution
waits for and consumes that same upload before color conversion. Superseded or overflowed frames release their leases;
the service reclaims them once in-flight GPU work finishes. Capture and upload continue without graph demand. The SDK's frame-sync layer is intentionally not placed in front of this common
timing path.

Both NDI nodes expose `alpha_mode`: `ignore`, `straight` (default), or `premultiplied`. On input, ignore treats RGB
as opaque regardless of the received alpha; straight decodes Rec.709 RGB and then premultiplies in linear light;
premultiplied first unpremultiplies the received video-space RGB, decodes it, then premultiplies in linear light.
All three produce UNORM16 linear premultiplied working images before downstream filtering/compositing. BGRX
sources are always opaque.

On output, straight unpremultiplies the working RGB before Rec.709 encoding. Premultiplied additionally multiplies
the encoded RGB by alpha. Ignore recovers the unpremultiplied colors and encodes them with alpha forced to one;
it does not flatten onto black. Zero-alpha RGB is unrecoverable and becomes black (opaque for ignore, transparent
otherwise). Alpha itself is never gamma-corrected. Quantization to RGBA8 occurs after conversion. The option is
read from the frame's graph snapshot without restarting the NDI sender/receiver; queued output frames retain their
already-rendered representation. Existing graphs without the option retain straight-alpha behavior. See
[NDI alpha modes](ndi-alpha-modes.md) for the exact conversions and verification.

NDI output renders frames carrying their absolute program target time in `utils::flicks` into a bounded RGBA readback
stream. Its worker
consumes completed leases in FIFO order, prerolls to the globally configured NDI-output buffer depth, and treats each
exact steady-clock send deadline as its output scheduling time, not an observed receiver presentation time. It
selects the closest PTS from the retained frame and queued frames, deliberately drops superseded program frames,
repeats the retained frame across missing intervals, skips obsolete output intervals rather than bursting to catch up,
and derives NDI timecode from the mapped program time. `clock_video` remains disabled. Potentially blocking asynchronous sends stay on the worker, and each readback
lease is retained until the following NDI async-send call releases the SDK's use of that memory. Enabled NDI outputs
remain demanding graph sinks regardless of receiver count. Like DeckLink output, the bounded readback stream queues its
initial slot set before sending begins rather than growing one retained slot at a time during playout. The sender worker
always drains completed readback leases into the bounded timed queue; when it falls behind, overflow disposal therefore
happens on that worker rather than exhausting render-thread slots. Preallocated pipeline headroom covers the two
consecutive evaluations possible under the current one-frame-late scheduler policy.

NDI and DeckLink outputs share `media::playout_timeline_s` with screen output. Preroll establishes buffered latency;
continuous actual-minus-predicted scheduling feedback adjusts frame selection independently of source-frame rounding.
DeckLink saves each scheduled frame's hardware-clock prediction for comparison when its completion arrives. NDI
measures local SDK handoff against its send deadline; this is not receiver presentation feedback. Actual-minus-selected
PTS remains a latency metric, not an accumulating cadence correction. SDK leases and hardware preroll are unchanged.

NDI and DeckLink input queues also compare the retained frame with the nearest queued PTS, after respecting startup
playout delay. Incoming samples are observed in source sequence order. Duplicate or delayed samples from an older
sequence/epoch are retired without rewinding the source clock; a new epoch still resets alignment. Transfers are
started before FIFO publication, and consumption still resolves the exact selected upload.

## Font registry and CPU surfaces

The font registry may refresh from the configuration thread. It uses a shared mutex and returns owned copies rather than pointers or views into its mutable map. Refresh increments `font_list_version_`; text and teleprompter nodes observe it, update status-backed font lists, and reload cached rendering.

Do not reintroduce pointer/view results whose lifetime crosses the registry lock.

`render::surface_s` is a non-owning CPU pixel span. Text and teleprompter rendering construct it over an upload lease,
so font work never owns GPU recordings and can run in the fiber pool. Copy and blend operations accept checked strided image
views, keeping storage extent, dimensions, and signed row stride together. Their templated helper clips once before pixel
loops; preserve the separation between clipping and pixel operations to avoid per-pixel boundary branches.

Surface-producing upload streams request `surface_s::DATA_ALIGNMENT`. The transfer factory verifies the exposed host
pointer for every backend, and `surface_s` uses that contract for compiler alignment hints. New surface producers must
carry the same requirement into their upload-stream configuration.

## Real-time queues and workers

`media::timed_source_queue_s<T>` aligns incoming frames to the program timeline, while
`media::timed_output_queue_s<T>` selects frames for presentation. Transfer services maintain their own bounded free,
pending, and in-flight slots. When no free slot exists, dropping a frame is generally preferable to blocking the
render thread.

Worker/callback rules:

- establish clear ownership when moving a frame between queues;
- complete GPU work before exposing host memory;
- stop/join workers before destroying referenced SDK objects or contexts;
- retain GPU resources until their submitted uses complete;
- avoid holding queue or registry locks across slow SDK calls.

## Key implementation files

- `src/gpu/window.hpp/.cpp`
- `src/gpu/texture.hpp/.cpp`
- `src/gpu/framebuffer.hpp/.cpp`
- `src/gpu/geometry.hpp`
- `src/gpu/textured_quad.hpp/.cpp`
- `src/gpu/device.hpp`, `recording.cpp`, and `presenter.cpp`
- `src/gpu/transfer/detail/frame_staging.hpp/.cpp`
- `src/gpu/transfer/texture_upload.hpp/.cpp`
- `src/gpu/transfer/texture_readback.hpp/.cpp`
- `src/gpu/transfer/detail/`
- `src/nodes/decklink/`
- `src/nodes/decklink/detail/input_capture.hpp/.cpp`
- `src/nodes/ndi/`
- `src/render/font/`
- `src/render/surface/`
- `src/media/timed_source_queue.hpp`
- `src/media/timed_output_queue.hpp`

## CEF implementation boundaries

CEF consumers receive `nodes::cef::session_s` for frame selection, status and trusted-native commands. Browser
creation, closure and retirement remain on the subsystem-owned internal session. Request destruction withdraws
publication and schedules retirement; existing consumers and frame leases finish before resources are reclaimed.
The browser lifecycle coordinates a bounded command channel and a capture stream, each owning its corresponding
state and synchronization. Shared typed message codecs contain the CEF process-list layout. GPU capture still waits
for every read of the borrowed native image before the callback returns.

CEF-enabled and unavailable nodes are selected by CMake and share option definitions. Frame-pool storage estimates
use the same format/sampling definition as actual allocation. Linux native imports use explicit scoped descriptor
ownership, releasing duplicated descriptors only when Vulkan accepts ownership.
