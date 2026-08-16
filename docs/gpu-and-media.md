# GPU and media architecture

## OpenGL context ownership

`gpu::context_s` wraps a GLFW OpenGL 4.6 context and maintains a thread-local current-context stack. Use `context_scope_s` to make a context current for a scope; its destructor rewinds the stack, including on early returns and exceptions. Re-entering the same current context is supported.

The render loop makes the root hidden context current before node preparation and rewinds it after `complete()`. Dedicated worker contexts share GL objects with their parent, but each context is owned by one native thread at a time:

```cpp
gpu::context_scope_s context_scope(*ctx);
// GL work or GL-owned destruction
```

Do not make one context current on multiple threads or run GL work in the fiber pool. Fibers produce or consume CPU data, while the render thread and dedicated transfer/display workers own GL contexts.

Textures, framebuffers, shaders, OpenGL fences, and transfer buffers assume an appropriate context is current for creation and destruction. Do not release them on arbitrary workers.

On Linux, GLFW is forced to X11 to obtain a GLX context. DVP requires GLX and may require a native NVIDIA Xorg session; XWayland may not expose required NVIDIA extensions. The current project is not configured around GLFW's EGL backend.

## Textures, framebuffers, and synchronization

`gpu::texture_s` owns a 2D texture and records display dimensions, storage dimensions, external pixel format/type, and color format. Storage and host formats are not necessarily byte-identical; OpenGL upload/readback operations may perform normalization or channel conversion.

`app_state_s` owns a small transparent RGBA16 fallback texture. Nodes whose operation requires a valid sampler, such
as a two-input mix, pass it explicitly as the fallback to texture-interface resolution. A missing texture otherwise
remains `nullptr`; absence can represent a disabled input, unavailable frame, or unselected switch branch and is not
globally converted into image data.

`gpu::framebuffer_s` owns a render target texture. Framebuffer values represent mutable ordered rendering and therefore have stricter graph fan-out rules than texture values.

Each framebuffer input interface owns a private fallback render target while disconnected. Its dimensions come from the
frame-local copy of `$app.default_framebuffer_size`, and its format is RGBA16F. The target is retained and cleared when
the input is resolved for each program frame. It is recreated on the render thread when the global size changes and
released there when the connected input is resolved. Inactive disconnected inputs perform no allocation or clearing.
Do not replace the fallback with a shared application framebuffer: framebuffer interfaces carry ordered mutable state
and therefore cannot safely share one fallback target.

`gpu::textured_quad_s` owns the standard textured-quad draw state. It sets the common rectangle and opacity uniforms,
binds and unbinds sampler zero, and submits the quad. Use a scoped batch for repeated draws so texture cleanup happens
once after the batch. Conversion paths may set their additional shader uniforms through the wrapper's shader accessor.
Nodes should not repeat the underlying texture-binding and quad-submission sequence.

Shared rectangle and scaling calculations live in `gpu/geometry.hpp`. Use its contain/cover operations for
aspect-preserving placement and pass texture display dimensions rather than storage dimensions. It also owns conversion
between pixel vectors and normalized draw coordinates, and from normalized node rectangles to pixel viewports; keep
this coordinate math out of individual nodes. Common rectangle interpolation and integer rounding belong there as
well.

`gpu::fence_s` owns the backend fence used by the current GPU implementation:

- `gpu_wait()` inserts a GPU-side wait;
- `cpu_wait()` blocks the caller up to a timeout;
- construction and destruction require GL current.

Cross-context texture frames own their synchronization. A producing worker attaches and flushes a ready fence; the
render context inserts a GPU wait before using the texture. In `complete()`, the consuming node attaches and flushes a
release fence. The render thread does not wait for that fence. Once the logical frame is retired, its worker waits
before returning the slot to the free queue or performing DVP/CUDA ownership transitions. Readbacks and display
handoffs likewise use explicit fences; there is no global GPU finish in the frame loop.

## Host/GPU transfer abstraction

Nodes use the app-owned `texture_upload_service_s` and `texture_readback_service_s`. The services contain an internal
`texture_transfer_backend_i` implementation selected by the texture-transfer backend factory:

- direction (`cpu_to_gpu` or `gpu_to_cpu`);
- host-buffer size and host-visible `host_memory()`;
- a bound texture and backend-specific registration;
- transfer to/from that texture;
- completion waiting;
- texture ownership transitions when required by DVP.

`texture_transfer_backend_i` is not a node-facing API. Upload/readback streams provide scheduling, pooling, memory
accounting, leases, and publication; the backend only implements one slot's host/GPU movement. Backend selection and
lifecycle are kept in `detail/texture_transfer_backend_factory`, rather than on the polymorphic interface.

Transfer capabilities are initialized once in `app_state_s` while the root GL context is current. DVP and CUDA are
initialized independently; the backend factory then selects a path for each stream from its direction, pixel format,
row stride, host access pattern, and alignment requirements:

1. NVIDIA DVP/GPU Direct for Video when the layout is supported and the texture registers successfully.
2. A CUDA image copy for formats whose host representation is proven identical to OpenGL storage.
3. A CUDA/OpenGL pixel buffer when OpenGL format conversion is required.
4. A persistent mapped OpenGL PBO fallback.

CUDA has no general query for arbitrary OpenGL format interoperability. General CUDA/GL support is detected during
initialization, while direct-image support combines the project-owned texture format description with registration of
the actual texture. Registration failure falls back without failing the stream. The format description is authoritative
for internal/external GL format, host and storage byte sizes, packed pixels, and direct-copy compatibility.

Upload and readback configurations contain `texture_transfer_layout_s`. Always describe the real
`host_row_stride_bytes`, `host_buffer_size_bytes`, and host-memory access pattern. Full-overwrite producers may receive
write-combined CUDA host memory; CPU rendering that reads and modifies existing pixels must request
`host_memory_access_e::read_write`.

The persistent fallback exposes its mapped unpack PBO directly as the upload lease's writable memory. Producers copy
into that mapping, and the upload worker flushes it before updating the texture; do not add a separate CPU staging
allocation in front of the PBO. Readback slots likewise expose their mapped pack PBO after readback completes.

### Upload streams

An upload stream is a bounded, lazily allocated set of backend-owned writable slots. A CPU producer calls
`try_acquire_upload_buffer()`, writes through `writable_host_bytes()`, then calls `submit()`. The upload worker owns a
permanently current shared GL context, performs the transfer, waits for its completion fence, and only then publishes
the texture. Legacy and non-timed producers may select with `select_latest_completed_upload()` or
`select_latest_completed_upload_through()` and retain their previous texture while a newer upload is incomplete. A
PTS-aware source instead calls `wait_for_upload()` for its exact prepared `texture_upload_id_s` during execution and
then selects it with `select_completed_upload()`. Exact selection also reclaims other completed slots that the source's
timing policy has made obsolete.

Upload consumption returns a retained texture-frame handle rather than a raw texture. The node GPU-waits on its ready
fence before exposing or sampling the texture and releases it from `complete()`. Replaced frames enter reclamation when
the timing/current-frame owner retires them, but the worker does not return a slot to the free queue, change external
ownership, overwrite it, or destroy it until the frame's latest release fence has signalled. Waiting can stall that
worker only when it is reclaiming an exhausted slot; bounded producers already drop when no free slot is available.

Submitted leases also pin their writable memory until the producer releases the lease. This is important for SDK allocators such as DeckLink, which may retain a buffer after delivering its frame callback.

Timed-source capacity applies across both callback-pending and render-aligned frames, not independently to each side
of the handoff. This keeps retained transfer memory at the advertised bound even when the render thread is delayed.

Use `acquire_upload_buffer_for()` only on native SDK threads that are allowed to wait for lazy slot allocation. Render
and fiber workers use `try_acquire_upload_buffer()` and yield/drop work when no slot is available.

### Readback streams

A readback stream owns bounded render-target/readback slots. The render thread calls `try_acquire_render_target()`,
renders into the target framebuffer, and calls `submit()`. Submission only inserts and flushes a fence. The readback
worker waits for rendering and performs the DVP/CUDA/PBO/basic readback on its shared context.

CPU consumers normally poll `try_consume_latest()`. PTS-aware consumers use `try_consume_oldest()` to retain FIFO
ordering and perform their own timed selection. A frame lease exposes `readable_host_bytes()` and keeps host memory
reserved until the external SDK has finished using it. If no render target is free, the node drops that output frame
instead of waiting.

Both services enforce memory budgets and catch allocation failures. Streams may request an initial bounded slot set when
their steady-state retention is known; those allocations still run asynchronously on the owning GL worker. Otherwise,
slots are allocated only after a stream is first used. Stream destruction reclaims its resources on that worker, and
per-stream slot limits bound latency and memory growth.

### Texture lifetime hooks

DVP needs textures registered and ownership coordinated between GL/API and DVP. Each backend instance registers one slot
texture for its complete lifetime and owns the corresponding registration state. Services call
`acquire_texture_for_gl()` and `release_texture_from_gl()` at established GL/transfer ownership boundaries; these
operations are no-ops for backends that do not require them. DVP handles are per slot rather than stored in a global
texture map.

### CUDA format rule

CUDA transfers normally register an OpenGL pixel buffer. CUDA copies between pinned host memory and the PBO; OpenGL performs any required format conversion while moving between the PBO and texture. Storage-identical readback formats may instead register the texture as a CUDA image and copy its array directly, avoiding an OpenGL readback and its driver-wide serialization.

This distinction is intentional. A texture's CUDA array reflects native storage, while host bytes normally use the texture's external format/type. For example, an RGBA8 host surface uploaded to `GL_RGBA16` requires OpenGL conversion; a raw CUDA-array copy would fill only half the row. Use direct image copies only for explicitly storage-identical formats.

### Completion and handoff

Completion is represented by the transfer service's publication state, not by queue ownership alone. An upload texture
is not visible through `select_latest_completed_upload()` or reported ready by `wait_for_upload()` until its transfer
fence has completed. A readback frame is not visible through `try_consume_latest()` until its host buffer is safe to
read. Queue
mutexes alone never imply GPU/DVP/CUDA completion.

Transfer shutdown runs from `app_state_s` with the root GL context current, after node transfers/textures are destroyed and before root-context destruction. DVP and CUDA context teardown must remain in that window.

## DeckLink

DeckLink input uses an `IDeckLinkVideoBufferAllocatorProvider` and `IDeckLinkVideoBufferAllocator` implementation. SDK
capture buffers hold upload leases, allowing DeckLink DMA to write directly into backend-owned host memory. The SDK
callback records the source PTS and frame metadata, detaches the write cycle's one-shot upload lease from the reusable
custom buffer, and stores that lease in a bounded timed-source queue. The custom buffer can therefore return to the SDK
allocator immediately, and the callback performs no GL work. During the active submission traversal, the render node
selects the frame assigned to the current program PTS and submits that exact lease. Execution waits for and consumes
that same upload ID before converting its UYUV texture. DeckLink may finish buffers in a different order from
their `StartAccess()` calls, so upload IDs identify transfer transactions rather than media order.

DeckLink may retain and reuse allocator buffer objects across captured frames rather than requesting a new object for
every frame. Each buffer therefore acquires a fresh one-shot upload lease from `StartAccess(bmdBufferAccessWrite)`.
`GetBytes()` exposes that lease's host address for the current SDK write cycle, and the selected frame ticket submits it.
Keeping one lease for the full lifetime of a DeckLink buffer would upload only its first frame and exhaust the transfer
slots after the initial pool. Completed GPU textures remain owned by the upload stream independently of the DeckLink
buffer object.

The custom allocator exposes at most eight reusable DeckLink buffer objects and reports `E_OUTOFMEMORY` when DeckLink
has all eight checked out; this is how the SDK establishes the bounded capture pool. Its upload stream preallocates
eight transfer slots and may grow to sixteen: four may be retained by timed selection, while the remainder cover the
published texture, DeckLink DMA writes, uploads, and asynchronous reclaim without coupling capture cadence to the
render thread. These limits are intentionally separate because DeckLink buffer-object reuse and transfer-slot lifetime
are independent. Every address still comes from a transfer backend; there is no node-owned staging allocation.

The upload stream retains its current completed texture until the render thread consumes a newer completed upload;
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
all capture buffers are returned, and transfer-stream destruction has been queued on the GL upload worker. Application
shutdown drains the DeckLink input-control worker before destroying the shared transfer services.

DeckLink output normally renders packed 10-bit YUV into a readback target carrying the frame's absolute program target
time in `utils::flicks`. Internal and external keyer modes instead preserve alpha through an RGBA16 intermediate and
read back DeckLink 8-bit ARGB bytes. The keyed path uses ARGB because Duo 2 keyed HD p60 scheduling accepts it where
the driver's otherwise supported BGRA path can reject the first scheduled frame. ARGB byte order is described by the
shared texture-transfer format, including DVP, CUDA pixel-buffer, and persistent OpenGL paths; it is not implemented as
a node-local transfer.
The transfer worker completes readback, and ready leases drain in FIFO order into a bounded timed-output queue. That
queue releases superseded or overflowed frames according to its explicit selection policy. Before playback starts, short
non-blocking control tasks collect actual program readbacks. Once the configured buffer target is available, those
program frames are scheduled as the SDK preroll and playback begins. One additional completed program frame remains in
the timed queue so bursty SDK completion callbacks do not make selection alternate between starvation and dropping a
newly completed batch. The completion callback then selects the newest
eligible program frame by mapping the next hardware presentation time into the absolute program clock. It explicitly
retains frames for repeats and accounts for superseded frames as timing drops. Both ordinary and keyed output use
`CreateVideoFrameWithBuffer` to wrap the transfer lease without a copy, so the SDK frame keeps host memory reserved
until DeckLink releases it.

The single global DeckLink-output buffer target defaults to four frames and is adjustable from one to eight. The SDK's
reported minimum preroll raises the effective target when necessary. All DeckLink output nodes use the same
frame-boundary snapshot. Changing the setting uses the normal asynchronous output restart and recreates each affected
bounded readback stream; a brief output interruption is expected. Output streams enqueue their complete bounded slot
set at creation so lazy pool growth cannot cause program-frame repeats after preroll begins. Neither the callback nor
the preroll control task makes a GL context current or waits for a transfer.

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
frame into a bounded unsubmitted upload lease, immediately frees the SDK frame, and pushes source timestamp, sequence,
duration, arrival observation, and NDI timing metadata through the same `media::timed_source_queue_s<T>` used by
DeckLink. The queue's shared media-to-program clock mapping converts the arbitrary sender timestamp origin into program
time.
All-node preparation advances that queue. Active graph submission starts the exact selected upload, and execution waits
for and consumes that same upload before color conversion. Superseded, overflowed, or inactive frames release their
host leases without starting GPU work. The SDK's frame-sync layer is intentionally not placed in front of this common
timing path.

NDI output renders frames carrying their absolute program target time in `utils::flicks` into a bounded RGBA readback
stream. Its worker
consumes completed leases in FIFO order, prerolls to the globally configured NDI-output buffer depth, and treats each
exact steady-clock send deadline as a physical presentation time. It deliberately drops superseded program frames,
repeats the retained frame across missing intervals, skips obsolete output intervals rather than bursting to catch up,
and derives NDI timecode from the mapped program time. `clock_video` remains disabled. Potentially blocking asynchronous sends stay on the worker, and each readback
lease is retained until the following NDI async-send call releases the SDK's use of that memory. Enabled NDI outputs
remain demanding graph sinks regardless of receiver count. Like DeckLink output, the bounded readback stream queues its
initial slot set before sending begins rather than growing one retained slot at a time during playout. The sender worker
always drains completed readback leases into the bounded timed queue; when it falls behind, overflow disposal therefore
happens on that worker rather than exhausting render-thread slots. Preallocated pipeline headroom covers the two
consecutive evaluations possible under the current one-frame-late scheduler policy.

## Font registry and CPU surfaces

The font registry may refresh from the configuration thread. It uses a shared mutex and returns owned copies rather than pointers or views into its mutable map. Refresh increments `font_list_version_`; text and teleprompter nodes observe it, update status-backed font lists, and reload cached rendering.

Do not reintroduce pointer/view results whose lifetime crosses the registry lock.

`render::surface_s` is a non-owning CPU pixel span. Text and teleprompter rendering construct it over an upload lease,
so font work never owns GL objects and can run in the fiber pool. Copy and blend operations accept checked strided image
views, keeping storage extent, dimensions, and signed row stride together. Their templated helper clips once before pixel
loops; preserve the separation between clipping and pixel operations to avoid per-pixel boundary branches.

Surface-producing upload streams request `surface_s::DATA_ALIGNMENT`. The transfer factory verifies the exposed host
pointer for every backend, and `surface_s` uses that contract for compiler alignment hints. New surface producers must
carry the same requirement into their upload-stream configuration.

## Real-time queues and workers

`utils::frame_queue_s<T>` is the standard mutex-protected FIFO containing a frame and flick timestamp. Real-time paths commonly maintain free, pending, and in-flight slots. When no free slot exists, dropping a frame is generally preferable to blocking the render thread.

Worker/callback rules:

- establish clear ownership when moving a frame between queues;
- complete GPU work before exposing host memory;
- stop/join workers before destroying referenced SDK objects or contexts;
- destroy GL-owned resources with their context current;
- avoid holding queue or registry locks across slow SDK calls.

## Key implementation files

- `src/gpu/context.hpp/.cpp`
- `src/gpu/texture.hpp/.cpp`
- `src/gpu/framebuffer.hpp/.cpp`
- `src/gpu/geometry.hpp`
- `src/gpu/textured_quad.hpp/.cpp`
- `src/gpu/fence.hpp/.cpp`
- `src/gpu/transfer/detail/texture_transfer_backend.hpp/.cpp`
- `src/gpu/transfer/detail/texture_transfer_backend_factory.hpp/.cpp`
- `src/gpu/transfer/texture_upload.hpp/.cpp`
- `src/gpu/transfer/texture_readback.hpp/.cpp`
- `src/gpu/transfer/detail/`
- `src/nodes/decklink/`
- `src/nodes/decklink/detail/input_capture.hpp/.cpp`
- `src/nodes/ndi/`
- `src/render/font/`
- `src/render/surface/`
- `src/utils/frame_queue.hpp`
