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

Preferred transfer selection is initialized once in `app_state_s` while the root GL context is current:

1. NVIDIA DVP/GPU Direct for Video when initialization succeeds.
2. CUDA/OpenGL interop.
3. Persistent mapped OpenGL PBO fallback.

### Upload streams

An upload stream is a bounded, lazily allocated set of backend-owned writable slots. A CPU producer calls `try_acquire()`, writes through the returned lease, then calls `submit()`. The upload worker owns a permanently current shared GL context, performs the transfer, waits for its completion fence, and only then publishes the texture. The render thread calls `consume_latest()` or `consume_through()`; both are polling operations and retain the previous texture while a newer upload is incomplete.

Submitted leases also pin their writable memory until the producer releases the lease. This is important for SDK allocators such as DeckLink, which may retain a buffer after delivering its frame callback.

Use `acquire_for()` only on native SDK threads that are allowed to wait for lazy slot allocation. Render and fiber workers use `try_acquire()` and yield/drop work when no slot is available.

### Download streams

A download stream owns bounded render-target/readback slots. The render thread calls `try_acquire()`, renders into the target framebuffer, and calls `submit()`. Submission only inserts and flushes a fence. The download worker waits for rendering and performs the DVP/CUDA/PBO/basic readback on its shared context.

CPU consumers poll `try_consume_latest()`. Its frame lease keeps host memory reserved until an NDI send or DeckLink copy has finished. If no render target is free, the node drops that output frame instead of waiting.

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

DeckLink output renders packed 10-bit YUV into a download target. The transfer worker completes readback, and the DeckLink scheduled-frame callback polls a CPU-frame lease and copies it into DeckLink-owned output memory. The callback neither makes a GL context current nor waits for a transfer.

DeckLink registry discovery is asynchronous and protected by a shared mutex. Device arrival/removal increments `device_list_version_`. Nodes compare that version before rebuilding device-name status lists.

SDK 16 buffer access uses `IDeckLinkVideoBuffer`: query it, call `StartAccess`, retrieve bytes, and call `EndAccess`. Linux SDK `REFIID` values do not provide normal C++ equality; follow the existing `QueryInterface` comparisons and COM pointer wrapper.

## NDI

The NDI registry runs a discovery thread, owns copied source names, protects them with a shared mutex, and increments `source_list_version_` after changes.

NDI input owns a dedicated capture thread. Each render tick coalesces a FrameSync sampling request; that worker performs the immediate FrameSync capture and copies into an upload lease. The render thread only signals, polls completed textures, and performs color conversion, preserving FrameSync time-base correction without a render-thread memory copy.

NDI output renders into a storage-identical RGBA download target. Its worker polls completed CPU-frame leases, performs potentially blocking asynchronous sends, and retains each lease until the following NDI async-send call releases the SDK's use of that memory.

## Font registry and CPU surfaces

The font registry may refresh from the configuration thread. It uses a shared mutex and returns owned copies rather than pointers or views into its mutable map. Refresh increments `font_list_version_`; text and teleprompter nodes observe it, update status-backed font lists, and reload cached rendering.

Do not reintroduce pointer/view results whose lifetime crosses the registry lock.

`render::surface_s` is a non-owning CPU pixel view. Text and teleprompter rendering construct it over an upload lease, so font work never owns GL objects and can run in the fiber pool. Its templated copy/blend helper clips once before pixel loops. Preserve the separation between clipping and pixel operations to avoid per-pixel boundary branches.

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
