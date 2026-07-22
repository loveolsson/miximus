# GPU and media architecture

## OpenGL context ownership

`gpu::context_s` wraps a GLFW OpenGL 4.6 context and maintains a thread-local current-context stack. Use `context_scope_s` to make a context current for a scope; its destructor rewinds the stack, including on early returns and exceptions. Re-entering the same current context is supported.

The render loop makes the root hidden context current before node preparation and rewinds it after `complete()`. Dedicated worker contexts share GL objects with their parent, but each context is owned by one native thread at a time:

```cpp
gpu::context_scope_s context_scope(*ctx);
// GL work or GL-owned destruction
```

Do not make one context current on multiple threads or run GL work in the fiber pool. Fibers produce or consume CPU data, while the render thread and dedicated transfer/display workers own GL contexts.

Textures, framebuffers, shaders, GL sync objects, and transfer buffers assume an appropriate context is current for creation and destruction. Do not release them on arbitrary workers.

On Linux, GLFW is forced to X11 to obtain a GLX context. DVP requires GLX and may require a native NVIDIA Xorg session; XWayland may not expose required NVIDIA extensions. The current project is not configured around GLFW's EGL backend.

## Textures, framebuffers, and synchronization

`gpu::texture_s` owns a 2D texture and records display dimensions, storage dimensions, external pixel format/type, and color format. Storage and host formats are not necessarily byte-identical; OpenGL upload/readback operations may perform normalization or channel conversion.

`gpu::framebuffer_s` owns a render target texture. Framebuffer values represent mutable ordered rendering and therefore have stricter graph fan-out rules than texture values.

`gpu::textured_quad_s` owns the standard textured-quad draw state. It sets the common rectangle and opacity uniforms,
binds and unbinds sampler zero, and submits the quad. Use a scoped batch for repeated draws so texture cleanup happens
once after the batch. Conversion paths may set their additional shader uniforms through the wrapper's shader accessor.
Nodes should not repeat the underlying texture-binding and quad-submission sequence.

Shared rectangle and scaling calculations live in `gpu/geometry.hpp`. Use its contain/cover operations for
aspect-preserving placement and pass texture display dimensions rather than storage dimensions. It also owns conversion
between pixel vectors and normalized draw coordinates, and from normalized node rectangles to pixel viewports; keep
this coordinate math out of individual nodes. Common rectangle interpolation and integer rounding belong there as
well.

`gpu::sync_s` wraps a GL fence:

- `gpu_wait()` inserts a GPU-side wait;
- `cpu_wait()` blocks the caller up to a timeout;
- construction and destruction require GL current.

The frame lifecycle currently performs one global `glFinish()` between all node `execute()` calls and all `complete()` calls. Transfer code must not rely on that finish: transfer services fence their own work and publish or recycle slots only after completion on a transfer/display worker.

## Host/GPU transfer abstraction

Nodes use the app-owned `texture_upload_service_s` and `texture_download_service_s`. The services contain an internal `backend_i` implementation selected by the backend factory:

- direction (`cpu_to_gpu` or `gpu_to_cpu`);
- byte size and host-visible `data()`;
- a bound texture and backend-specific registration;
- transfer to/from that texture;
- completion waiting;
- texture ownership transitions when required by DVP.

`backend_i` is not a node-facing API. Upload/download streams provide scheduling, pooling, memory accounting, leases, and publication; the backend only implements one slot's host/GPU movement. Backend selection and lifecycle are kept in `detail/backend_factory`, rather than on the polymorphic interface.

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

Upload and download descriptions contain `texture_transfer_requirements_s`. Always describe the real row stride and
host access pattern. Full-overwrite producers may receive write-combined CUDA host memory; CPU rendering that reads and
modifies existing pixels must request `host_access_e::read_write`.

The persistent fallback exposes its mapped unpack PBO directly as the upload lease's writable memory. Producers copy
into that mapping, and the upload worker flushes it before updating the texture; do not add a separate CPU staging
allocation in front of the PBO. Download slots likewise expose their mapped pack PBO after readback completes.

### Upload streams

An upload stream is a bounded, lazily allocated set of backend-owned writable slots. A CPU producer calls `try_acquire()`, writes through the returned lease's mutable byte span, then calls `submit()`. The upload worker owns a permanently current shared GL context, performs the transfer, waits for its completion fence, and only then publishes the texture. The render thread calls `consume_latest()` or `consume_through()`; both are polling operations and retain the previous texture while a newer upload is incomplete.

Submitted leases also pin their writable memory until the producer releases the lease. This is important for SDK allocators such as DeckLink, which may retain a buffer after delivering its frame callback.

Use `acquire_for()` only on native SDK threads that are allowed to wait for lazy slot allocation. Render and fiber workers use `try_acquire()` and yield/drop work when no slot is available.

### Download streams

A download stream owns bounded render-target/readback slots. The render thread calls `try_acquire()`, renders into the target framebuffer, and calls `submit()`. Submission only inserts and flushes a fence. The download worker waits for rendering and performs the DVP/CUDA/PBO/basic readback on its shared context.

CPU consumers poll `try_consume_latest()`. Its frame lease exposes a read-only byte span and keeps host memory reserved until an NDI send or DeckLink copy has finished. If no render target is free, the node drops that output frame instead of waiting.

Both services enforce memory budgets and catch allocation failures. Slots are allocated only after a stream is first used, and stream destruction reclaims its resources on the owning GL worker. Per-stream slot limits bound latency and memory growth.

### Texture lifetime hooks

DVP needs textures registered and ownership coordinated between GL/API and DVP. Each backend instance binds one slot texture for its complete lifetime and owns the corresponding registration state. Services call `begin_texture_use()` and `end_texture_use()` at established GL/transfer ownership boundaries; these operations are no-ops for backends that do not require them. DVP handles are per slot rather than stored in a global texture map.

### CUDA format rule

CUDA transfers normally register an OpenGL pixel buffer. CUDA copies between pinned host memory and the PBO; OpenGL performs any required format conversion while moving between the PBO and texture. Storage-identical download formats may instead register the texture as a CUDA image and copy its array directly, avoiding an OpenGL readback and its driver-wide serialization.

This distinction is intentional. A texture's CUDA array reflects native storage, while host bytes normally use the texture's external format/type. For example, an RGBA8 host surface uploaded to `GL_RGBA16` requires OpenGL conversion; a raw CUDA-array copy would fill only half the row. Use direct image copies only for explicitly storage-identical formats.

### Completion and handoff

Completion is represented by ownership, not a node-side wait. An upload texture is not visible through `consume_latest()` until its transfer fence has completed. A download frame is not visible through `try_consume_latest()` until its host buffer is safe to read. Queue mutexes alone never imply GPU/DVP/CUDA completion.

Transfer shutdown runs from `app_state_s` with the root GL context current, after node transfers/textures are destroyed and before root-context destruction. DVP and CUDA context teardown must remain in that window.

## DeckLink

DeckLink input uses an `IDeckLinkVideoBufferAllocatorProvider` and `IDeckLinkVideoBufferAllocator` implementation. SDK capture buffers hold upload leases, allowing DeckLink DMA to write directly into backend-owned host memory. The SDK callback records frame metadata and submits the lease; it performs no GL work. The render node polls completed UYUV textures and retains the stream while sampling one.

DeckLink cycles a fixed set of allocator buffer objects rather than allocating an object for every captured frame. Each
buffer therefore acquires a fresh one-shot upload lease from `StartAccess(bmdBufferAccessWrite)`. `GetBytes()` exposes
that lease's host address for the current SDK write cycle, and the frame callback submits it. Keeping one lease for the
full lifetime of a DeckLink buffer would upload only its first frame and exhaust the transfer slots after the initial
pool. Completed GPU textures remain owned by the upload stream independently of the DeckLink buffer object.

DeckLink wraps application-provided buffers in its input-frame objects. Upload-backed buffers therefore expose a private
IID, matching the SDK custom-allocator examples, so the callback can recover the original buffer and submit its lease.
The registry owns a serialized DeckLink input-control worker. Capture start, stop, flush, disable, device removal, and
allocator retirement all run there; the render thread never calls those potentially blocking SDK methods. A format
callback only records the pending mode and asks the render node to release its current texture. The next render prepare
acknowledges that release without waiting, after which the control worker stops capture and waits for SDK buffer
references to drain. Only then is the old allocator and upload stream retired and capture enabled at the pending mode.
Old and new full-size allocator pools never coexist.

Input-node destruction is likewise non-blocking: it clears render-owned textures, requests asynchronous capture stop,
and releases its callback reference. Control tasks retain the callback and device until the SDK callback is unregistered,
all capture buffers are returned, and transfer-stream destruction has been queued on the GL upload worker. Application
shutdown drains the DeckLink input-control worker before destroying the shared transfer services.

DeckLink output renders packed 10-bit YUV into a download target. The transfer worker completes readback, and the DeckLink scheduled-frame callback polls a CPU-frame lease and copies it into DeckLink-owned output memory. The callback neither makes a GL context current nor waits for a transfer.

DeckLink registry discovery is asynchronous and protected by a shared mutex. Device arrival/removal increments `device_list_version_`. Nodes compare that version before rebuilding device-name status lists.

The registry also owns one status monitor per physical device. DeckLink `bmdStatusChanged` notifications update an
owned plain-data snapshot, while a single registry worker polls non-notifiable hardware statistics such as temperature.
Nodes copy snapshots by device name and publish them only when the snapshot version changes. Stream-specific queue
depth and frame outcome counters remain on the input/output callbacks and are throttled to status once per second.
No status or statistics SDK query runs on the render thread.

All application-provided DeckLink callbacks and buffers are complete `IUnknown` implementations: `QueryInterface` must return only matching interfaces, reference counts are atomic, and exceptions must be caught before crossing an SDK callback boundary. Input shutdown stops capture and unregisters the callback before releasing the node's references. Output shutdown calls `StopScheduledPlayback` and retains the callback and device until `ScheduledPlaybackHasStopped`; normal render ticks poll this state instead of waiting on the render thread. A bounded timeout handles devices that disappear without delivering the final callback.

SDK 16 buffer access uses `IDeckLinkVideoBuffer`: query it, call `StartAccess`, retrieve bytes, and call `EndAccess`. Linux SDK `REFIID` values do not provide normal C++ equality; follow the existing `QueryInterface` comparisons and COM pointer wrapper.

## NDI

The NDI registry runs a discovery thread, owns copied source names, protects them with a shared mutex, and increments `source_list_version_` after changes.

NDI input owns a dedicated capture thread. Each render tick coalesces a FrameSync sampling request; that worker performs the immediate FrameSync capture and copies into an upload lease. The render thread only signals, polls completed textures, and performs color conversion, preserving FrameSync time-base correction without a render-thread memory copy.

NDI output renders into a storage-identical RGBA download target. Its worker polls completed CPU-frame leases, performs potentially blocking asynchronous sends, and retains each lease until the following NDI async-send call releases the SDK's use of that memory.

## Font registry and CPU surfaces

The font registry may refresh from the configuration thread. It uses a shared mutex and returns owned copies rather than pointers or views into its mutable map. Refresh increments `font_list_version_`; text and teleprompter nodes observe it, update status-backed font lists, and reload cached rendering.

Do not reintroduce pointer/view results whose lifetime crosses the registry lock.

`render::surface_s` is a non-owning CPU pixel span. Text and teleprompter rendering construct it over an upload lease,
so font work never owns GL objects and can run in the fiber pool. Copy and blend operations accept checked strided image
views, keeping storage extent, dimensions, and signed row stride together. Their templated helper clips once before pixel
loops; preserve the separation between clipping and pixel operations to avoid per-pixel boundary branches.

Surface-producing upload streams request `surface_s::DATA_ALIGNMENT`. The transfer factory verifies the exposed host
pointer for every backend, and `surface_s` uses that contract for compiler alignment hints. New surface producers must
carry the same requirement into their upload-stream description.

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
- `src/gpu/sync.hpp/.cpp`
- `src/gpu/transfer/detail/backend.hpp/.cpp`
- `src/gpu/transfer/detail/backend_factory.hpp/.cpp`
- `src/gpu/transfer/texture_upload.hpp/.cpp`
- `src/gpu/transfer/texture_download.hpp/.cpp`
- `src/gpu/transfer/detail/`
- `src/nodes/decklink/`
- `src/nodes/ndi/`
- `src/render/font/`
- `src/render/surface/`
- `src/utils/frame_queue.hpp`
