# GPU and media architecture

## OpenGL context ownership

`gpu::context_s` wraps a GLFW OpenGL 4.6 context and maintains a thread-local current-context stack. Use `context_scope_s` to make a context current for a scope; its destructor rewinds the stack, including on early returns and exceptions. Re-entering the same current context is supported.

The render loop makes the root hidden context current before node preparation and rewinds it after `complete()`. Shared worker contexts share objects with a parent and require external locking:

```cpp
gpu::context_scope_s context_scope(*ctx, gpu::context_lock_e::lock);
// GL work or GL-owned destruction
```

The default `context_scope_s` does not take the context mutex and is appropriate for thread-owned contexts. Pass `context_lock_e::lock` for contexts used by callbacks or multiple threads. `context_s::get_lock()` remains available for synchronized operations that do not need a GL context current.

Textures, framebuffers, shaders, GL sync objects, and transfer buffers assume an appropriate context is current for creation and destruction. Do not release them on arbitrary workers.

On Linux, GLFW is forced to X11 to obtain a GLX context. DVP requires GLX and may require a native NVIDIA Xorg session; XWayland may not expose required NVIDIA extensions. The current project is not configured around GLFW's EGL backend.

## Textures, framebuffers, and synchronization

`gpu::texture_s` owns a 2D texture and records display dimensions, storage dimensions, external pixel format/type, and color format. Storage and host formats are not necessarily byte-identical; OpenGL upload/readback operations may perform normalization or channel conversion.

`gpu::framebuffer_s` owns a render target texture. Framebuffer values represent mutable ordered rendering and therefore have stricter graph fan-out rules than texture values.

`gpu::sync_s` wraps a GL fence:

- `gpu_wait()` inserts a GPU-side wait;
- `cpu_wait()` blocks the caller up to a timeout;
- construction and destruction require GL current.

The frame lifecycle also performs one global `glFinish()` between all node `execute()` calls and all `complete()` calls. Use `complete()` for handoffs that require frame GPU work to be finished.

## Host/GPU transfer abstraction

Use `gpu::transfer::transfer_i` instead of writing node-specific PBO/readback paths. Transfers expose:

- direction (`cpu_to_gpu` or `gpu_to_cpu`);
- byte size and host-visible `ptr()`;
- `perform_copy()`;
- transfer to/from a texture or framebuffer;
- `wait_for_copy()`;
- backend identity through virtual `type()`.

Preferred transfer selection is initialized once in `app_state_s` while the root GL context is current:

1. NVIDIA DVP/GPU Direct for Video when initialization succeeds.
2. CUDA/OpenGL interop.
3. Persistent mapped OpenGL PBO fallback.

The basic synchronous backend remains available explicitly.

### Texture lifetime hooks

DVP needs textures registered and ownership coordinated between GL/API and DVP. Pair registration over the complete texture lifetime:

```cpp
transfer_i::register_texture(transfer->type(), texture);
// use texture and transfer
transfer_i::unregister_texture(transfer->type(), texture);
```

Use `begin_texture_use()` and `end_texture_use()` at the established GL/transfer ownership boundaries. These hooks are no-ops for backends that do not need them. Prefer an existing transfer's `type()` to querying global preference again.

### CUDA format rule

CUDA transfers register an OpenGL pixel buffer. CUDA copies between pinned host memory and the PBO; OpenGL uses `glTextureSubImage2D`, `glGetTexImage`, or `glReadPixels` to move between the PBO and texture.

This design is intentional. A texture's CUDA array reflects native storage, while host bytes use the texture's external format/type. For example, an RGBA8 host surface uploaded to `GL_RGBA16` requires OpenGL conversion. A raw CUDA-array copy would fill only half the row. Do not bypass the PBO conversion path unless formats are proven storage-identical.

### Completion and handoff

Before a non-GL worker reads a GPU-to-CPU transfer's `ptr()`, call `wait_for_copy()`. Queue mutexes provide memory ordering but do not complete GPU/DVP/CUDA work.

CPU-to-GPU callers should preserve the established backend-independent sequencing around `perform_copy()`, `wait_for_copy()`, `perform_transfer()`, and texture ownership hooks. Representative users are:

- DeckLink input custom allocator/callback;
- NDI input upload;
- NDI output readback and send worker;
- `render::surface_s` used by text and teleprompter.

Transfer shutdown runs from `app_state_s` with the root GL context current, after node transfers/textures are destroyed and before root-context destruction. DVP and CUDA context teardown must remain in that window.

## DeckLink

DeckLink input uses an `IDeckLinkVideoBufferAllocatorProvider` and `IDeckLinkVideoBufferAllocator` implementation. SDK capture buffers wrap transfer-owned host memory, allowing DeckLink to write into the selected transfer backend. The callback owns a shared GL context, uploads captured UYUV frames, and passes synchronized textures to the render node.

DeckLink registry discovery is asynchronous and protected by a shared mutex. Device arrival/removal increments `device_list_version_`. Nodes compare that version before rebuilding device-name status lists.

SDK 16 buffer access uses `IDeckLinkVideoBuffer`: query it, call `StartAccess`, retrieve bytes, and call `EndAccess`. Linux SDK `REFIID` values do not provide normal C++ equality; follow the existing `QueryInterface` comparisons and COM pointer wrapper.

## NDI

The NDI registry runs a discovery thread, owns copied source names, protects them with a shared mutex, and increments `source_list_version_` after changes.

NDI input captures frames, copies them into transfer memory, uploads to a registered texture, and converts gamma/YUV representation with GL shaders.

NDI output renders into a BGRA framebuffer and initiates GPU-to-CPU readback on the render thread. `complete()` waits for backend completion and enqueues host memory. A dedicated worker performs potentially blocking asynchronous NDI sends and recycles frame slots. Keep SDK sends off the render thread.

## Font registry and CPU surfaces

The font registry may refresh from the configuration thread. It uses a shared mutex and returns owned copies rather than pointers or views into its mutable map. Refresh increments `font_list_version_`; text and teleprompter nodes observe it, update status-backed font lists, and reload cached rendering.

Do not reintroduce pointer/view results whose lifetime crosses the registry lock.

`render::surface_s` owns CPU RGBA pixels, a GPU texture, and a CPU-to-GPU transfer. Its templated copy/blend helper clips once before pixel loops. Preserve the separation between clipping and pixel operations to avoid per-pixel boundary branches.

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
- `src/gpu/transfer/transfer.hpp/.cpp`
- `src/gpu/transfer/detail/`
- `src/nodes/decklink/`
- `src/nodes/ndi/`
- `src/render/font/`
- `src/render/surface/`
- `src/utils/frame_queue.hpp`
