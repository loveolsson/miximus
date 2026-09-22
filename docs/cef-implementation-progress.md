# CEF implementation progress

This records implementation evidence for the [accelerated-only plan](cef-browser-sources.md), not permission to
change existing rendering. CPU pixel transport, a browser-host process, and render/event-loop changes remain excluded.

## Stage 0: existing-structure fit

Implemented a private [GPU destination pool](../src/nodes/cef/detail/frame_pool.hpp) under the CEF module. It allocates
a fixed number of ordinary RGBA UNORM16 textures, rejects generations above a configured texture-payload budget,
and hands out reference-counted frame leases. The budget is an image-size estimate, not a bound on driver allocation
overhead or on all simultaneously retiring generations; the eventual session resource worker must also bound those.

Lease release alone does not free a slot for writing: `texture_s::idle()` must also establish retirement of all
recorded/submitted GPU uses. Frames retain their old pool generation across resize/removal. All slot lease flags are
protected by a private mutex; textures and the slot vector are immutable after allocation. Producer recording and
graph consumption use the existing independently owned recording contexts and existing submission/completion code.
The producer publishes a const frame lease only after its required GPU work finishes. No CPU pixels are copied.

The module is built as an isolated library and hardware-test executable. It is not linked into the application yet;
no browser source is registered and no CEF runtime is initialized. The private native-image import qualification
helper described below is not connected to browser callbacks.
This stage adds the CEF module to the node CMake subdirectory list. The separately approved device capability
change is described below. Shader operations, transfer backends, renderer, scheduler, main loop, app-state and
existing nodes remain unchanged. The eventual source uses `media::timed_source_queue_s<T>`; this texture pool
does not replace that queue. Existing transfer/presentation pools have specialized embedded lifecycles; they
have not been refactored to introduce a generic pool during CEF integration.

### Verification on 2026-09-22

- `cmake --build build -j`: passed.
- Ordinary CTest: 135/135 passed.
- New `cef_ingress_vulkan_test`: 5/5 passed with Vulkan synchronization validation.
- `gpu_vulkan_test`: 30/30 passed with Vulkan synchronization validation, including two new device capability tests.
- Existing `gpu_transfer_vulkan_test`: 14/14 passed with Vulkan validation.
- Hardware: NVIDIA Quadro P2000, driver 580.178.04. No validation errors reported by the new suite.

The new tests cover retained-frame pool exhaustion, lease release with an unsubmitted consumer still referencing
the image, GPU-only producer work on a separate thread/context followed by consumer retirement, old-generation
lifetime after pool replacement, and invalid/over-budget configurations. They use ordinary Vulkan textures as source
fixtures; they do **not** establish CEF native-resource readiness, format compatibility, color correctness or runtime
packaging. No screen, NDI or DeckLink live-output behavior has been requalified by these tests.

## Approved and implemented: optional Linux device extension enablement

The proposed next native-image path imports DMA-BUFs into the **existing selected Vulkan device**. Previously,
[`device.cpp`](../src/gpu/device.cpp) enabled external FD memory/semaphore extensions only as part of requested CUDA
support, and did not enable the DMA-BUF/modifier extensions. A new recording context cannot enable device extensions
after logical-device creation. Vulkan specifies the enabled extension list in
[`VkDeviceCreateInfo`](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html).

The user explicitly approved the following bounded initialization change, which is now implemented:

1. Add a default-off `external_image_import` request to `gpu::device_options_s`. Initially only an explicit CEF
   qualification target requests it; later CEF-enabled app-state construction can request it through normal wiring.
2. For the device chosen by the existing selection policy, probe a contained Linux import capability set:
   `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
   `VK_EXT_queue_family_foreign`, and `VK_KHR_external_semaphore_fd` for the synchronization experiment.
3. If requested and supported, append these names without duplicates to the existing logical-device extension list.
   If unavailable, report import capability unavailable. Do not reject an otherwise usable device, pick another GPU,
   change queue selection/count, request CUDA, or change behavior for callers leaving the new request disabled.
4. Expose an owned capability result for the contained importer. Keep platform import/FD ownership and external
   synchronization helpers separate from the graph and existing transfer implementations.

This modifies initialization capability policy in `src/gpu/device.hpp`, `src/gpu/device.cpp` and private device state,
under that specific approval. It is not authorization for any other device/queue/renderer restructuring.
The application leaves the option disabled; only the new qualification tests request it. Pure tests cover the
disabled request, each missing required extension, and the complete set. Hardware tests verify unchanged selected
GPU and reported queue/presentation capabilities, ordinary GPU rendering with import extensions enabled, and
coexistence with the CUDA request without validation errors. The local P2000 supports all five extensions, but this
does not prove that a particular CEF image/modifier is importable or that its producer synchronization is sufficient.

Next, qualify actual handle import and ownership using isolated GPU tests; then connect CEF callbacks
to the existing pool/recording/completion pattern. Source readiness is still unresolved: CEF's Linux descriptor lacks
an explicit acquire sync FD, so determine the applicable capture/driver contract before submitting any borrowed read.
Do not infer readiness from handle delivery or import success. A callback may not return while its borrowed GPU read
is still in flight. Any additional structural dependency gets its own explicit approval request.

## Linux DMA-BUF image import qualification

Added a private GPU helper in [`dma_buf_image.hpp`](../src/gpu/detail/dma_buf_image.hpp) and its implementation.
It creates a sampled-only, one-mip image from a borrowed DMA-BUF descriptor. The initial supported subset is
single-memory-plane RGBA8/BGRA8 with an explicitly supported DRM modifier and filtered sampling. Unsupported
modifiers, layouts and channel orders fail; there is no CPU path. The helper checks the selected device's exact
format/modifier import properties, intersects image and FD memory-type requirements, duplicates the borrowed FD,
and transfers only that duplicate to Vulkan on successful allocation. Allocation and image-view cleanup use the
existing `texture_state_s` retirement mechanism. No existing public GPU API, recording/submission logic or node
behavior changed. Linux-only sources and a separate hardware-test target are added in the GPU CMake file.

The implementation follows Vulkan's [explicit modifier layout contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageDrmFormatModifierExplicitCreateInfoEXT.html)
and [FD ownership contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html).
The caller must establish that the descriptor belongs to a compatible producer on the same physical GPU; the FD
alone does not establish CEF adapter identity. A sampled BGRA view performs the native component interpretation;
this step does not implement browser color-space or alpha conversion.

`gpu_dma_buf_vulkan_test` exercises actual Vulkan-exported DMA-BUF allocations, without pixel readback:

- RGBA import twice, then destruction of both imports while the original borrowed FD remains valid.
- BGRA import followed by closing the source FD and releasing the exporting allocation.
- Rejection of unsupported modifier/channel order and invalid stride, followed by a valid import.

All three tests passed on the P2000 with Vulkan synchronization validation. These tests establish allocation/handle
lifetime only: they submit no GPU reads of the imported image, and do not establish pixel correctness, foreign
ownership transfer, CEF readiness, cross-process operation, other GPUs, or multi-plane modifier support.

The importer exposes only private GPU state. Importing an FD does not make it a graph-consumable texture.

## Linux fence and GPU copy helper

Added [`dma_buf_copy_s`](../src/gpu/detail/dma_buf_copy.hpp), a private ingress helper that:

1. Imports the image and exports its currently published write fences with `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`.
2. Waits for that exact fence snapshot within an explicit caller-provided readiness budget, off the render thread.
   Timeout fails before enqueueing GPU work. Imports the signalled payload into a temporary binary Vulkan semaphore,
   after checking SYNC_FD import support.
3. Records foreign-queue ownership acquire, an existing typed GPU draw into an owned destination, and ownership
   release back to the foreign producer in GENERAL layout.
4. Enqueues through the existing private submission entry point with the native semaphore wait, returning the
   ordinary `completion_s`. Imported image and semaphore lifetimes use existing recording/timeline retirement.

The helper adds friendship to the existing recording/texture classes, following the private CUDA bridge pattern.
It does not alter recording, submission, queue selection, shaders, rendering, graph evaluation, or existing nodes.
The graph never receives the borrowed imported image. This helper is not yet called by a browser or application node.
All pixels remain on the GPU; there is no image mapping, host staging or readback. The caller supplies existing draw
conversion parameters; CEF-specific sRGB/premultiplied-alpha conversion remains to be implemented and verified.

The borrow must remain exclusive until the returned completion is ready. An error requires abandoning the recording.
A completion-wait timeout does not cancel GPU work and never permits returning the producer's borrowed image early.
The helper polls the exported fence before queue submission, within the supplied budget, so it does not submit an
unresolved producer wait onto the shared graphics queue. Fence errors fail the import. This is off-render-thread
control waiting, not a CPU pixel copy. CEF still needs qualification of the producer fence-publication contract.
No existing queue or submission behavior is changed.

The hardware suite now has five tests. Two additions use a separate logical Vulkan device on the same physical GPU
as a controlled external producer. It GPU-clears the source, releases ownership and publishes a write fence to the
DMA-BUF reservation object; the consumer uses its own ordinary recording context and existing submission worker.
They exercise GPU-copy completion/resource retirement and failure after importing resources followed by abandonment
and successful reuse of the recording capacity. All five pass with synchronization validation on the P2000.
The test may observe an already-signalled producer fence; it does not prove delayed-producer behavior. No pixel
readback comparison is performed, so this establishes execution/lifetime validity, not color correctness.

The implementation uses the documented [DMA-BUF fence interface](https://docs.kernel.org/driver-api/dma-buf.html)
and [temporary SYNC_FD import](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportSemaphoreFdInfoKHR.html).
**CEF producer readiness remains unqualified:** exporting a reservation fence is sufficient only if the actual
Chromium/driver path publishes the relevant write fence before callback delivery. These controlled tests do not
establish that fact, CEF adapter identity, cross-process behavior or support on Mesa/other hardware. Unsupported
synchronization must leave this path unavailable; it must never trigger unsynchronized reads or CPU transport.

## SDK preparation

Rechecked the official build index and downloaded the selected Linux x64 **standard** SDK to `/tmp`, outside Git.
Its SHA-1 matches the official index. Recorded digest for subsequent wrapper integration:

```text
Version: 152.0.8+g1ce985c+chromium-152.0.7977.134
Archive: cef_binary_152.0.8+g1ce985c+chromium-152.0.7977.134_linux64.tar.bz2
Size: 674894043 bytes
SHA-1: add0a51f7333bc660e8e3bafd998e0122568f7d8
SHA-256: 4967293a608424b98f2ff4ab15f4119064de966018df6458a80f2a7073dd1dc0
```

Source: [CEF build index](https://cef-builds.spotifycdn.com/index.json). No CEF SDK/runtime code has been built or
installed into the app, and no claims about end-to-end accelerated browser support follow from this download.
Windows GPU qualification and macOS event-loop placement remain open platform gates.
