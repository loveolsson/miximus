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
no browser source is registered, no CEF runtime is initialized, and no native-image import has been implemented.
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
