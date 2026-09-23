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

Source: [CEF build index](https://cef-builds.spotifycdn.com/index.json).
Windows GPU qualification and macOS event-loop placement remain open platform gates.

## Optional SDK wrapper and runtime qualification

The pinned Linux x64 SDK now builds through `src/wrapper/cef/`, behind `MIXIMUS_ENABLE_CEF` (default off).
`sdk.json` records the exact version, API version and archive digest; `acquire.cmake` explicitly downloads/verifies
and extracts it. Configure never downloads the SDK implicitly. Other platforms remain unavailable until their
artifacts and integration are qualified. The wrapper stages the SDK runtime, resources, licenses and a minimal
`CefExecuteProcess` helper. It builds the matching upstream C++ wrapper with upstream warning settings.

The contained `cef_runtime` uses CEF's supported threaded message loop with sandboxing enabled. Chromium alone
receives `ozone-platform=x11`, matching the application's existing X11/XWayland environment, and `use-angle=gl-egl`,
the [documented Linux shared-texture requirement](https://github.com/chromiumembedded/cef/issues/3953). Chromium first-run
onboarding and default-browser checks are disabled: fresh profiles previously displayed an agreement dialog and
blocked initialization. A fresh-profile runtime probe now initializes and shuts down successfully without that
dialog, with Vulkan validation enabled. No GLFW, graph or render-loop changes are involved.

CEF must be a direct executable dependency for its Linux `close` interceptor. The executable's loader path exposes
only `libcef.so` and resource symlinks in `cef-link/`; it does not expose CEF's bundled Vulkan/ANGLE libraries.
CEF children use the full `cef/` runtime directory. The runtime probe verified that Miximus retained the system
Vulkan loader before and after CEF initialization. Resource symlinks are necessary because Chromium locates ICU
relative to the loaded library even when `resources_dir_path` is supplied.

The wrapper install rules were also exercised into an isolated prefix. Relative `cef-link/` links, including the
locales directory link, were preserved and the installed helper retained an `$ORIGIN` RUNPATH. A relocated runtime
probe with its ordinary Miximus resource library initialized and shut down against that installed stock bundle with
a fresh profile, no agreement dialog, and zero Vulkan validation errors; it retained the system Vulkan loader.
This verifies the local bundle layout, not a clean-machine dependency audit or the still-building patched artifact.
Staging now depends on the runtime binaries and resource files themselves, and includes patched-build provenance
when present. A manual check changing only the SDK library timestamp retriggered staging; the native build passed.
This prevents a same-version patched library from being missed merely because its version header did not change.

Two explicit probes are available when CEF and testing are enabled:

```sh
cmake -S . -B build -DMIXIMUS_ENABLE_CEF=ON -DMIXIMUS_CEF_ROOT=/path/to/pinned/sdk
cmake --build build -j
build/src/nodes/cef/cef_runtime_probe "$PWD/build/cef" /tmp/miximus-cef-runtime-profile
build/src/nodes/cef/cef_accelerated_probe "$PWD/build/cef" /tmp/miximus-cef-capture-profile
```

These are manual hardware probes, not ordinary CTest cases. The accelerated probe requests shared textures,
rejects software paint without ingesting pixels, and attempts the private GPU copy helper on accelerated delivery.
It now requires 120 completed GPU copies from an animated, partially transparent page with nondecreasing capture
timestamps, rather than accepting a single callback. This strengthened probe builds successfully; its passing
hardware result remains pending the patched SDK. It does not infer pixel correctness from callback count.
Errors arriving after the frame-count target but before browser closure still fail the probe. CEF shutdown completes
before the final Vulkan validation-count check, while the device and its validation callback remain alive.
**End-to-end capture is not yet qualified:** on the local P2000, Chromium creates the browser but cannot initialize
the capture SkSurface. Adapter identity, producer fence
publication and color conversion also remain qualification gates. No CEF node or app subsystem has been wired up
yet. SDK/runtime build success must not be interpreted as completion of browser-source support.

### NVIDIA capture gate: upstream native-handle allocation

Initially, without `use-angle=gl-egl`, the probe reported a denied GBM driver load. A diagnostic run preloading driver
libraries passed that point but failed to produce a compatible GL representation. No preload workaround was retained.
Using the documented EGL setting, with no preload and the sandbox still enabled, instead reaches
`SharedImageRepresentation: Unable to initialize SkSurface`; no accelerated paint arrives within 15 seconds.

The failure is consistent with [CEF issue 4237](https://github.com/chromiumembedded/cef/issues/4237). The pinned
`libcef/browser/osr/video_consumer_osr.cc::SetActive` explicitly selects `kPreferMappableSharedImage`. On Linux this
requests CPU-mappable, linear GBM allocations, which the upstream report identifies as incompatible with this NVIDIA
rendering path. This does not mean Miximus copies pixels through the CPU; allocation policy inside Chromium prevents
the accelerated callback from arriving in the first place.

[CEF PR 4238](https://github.com/chromiumembedded/cef/pull/4238) proposes selecting
`kPreferSharedImageWithNativeHandle` on Linux, retaining the existing preference on other platforms. It also requires
Chromium's frame-sink capturer to permit its GPU blit for that preference, addressed by
[Chromium change 8220427](https://chromium-review.googlesource.com/c/chromium/src/+/8220427).
As checked on 2026-09-22, the CEF PR remains open; its September 15 discussion reports the Chromium change merged
and the CEF maintainer plans to wait for a subsequent Chromium roll, probably M156. That is not a fix in our pinned
152 SDK. Matching the report is evidence of a likely cause, not hardware validation of the proposed fix.

The concrete next SDK decision is whether to maintain a Linux-only build of the pinned stable CEF with both changes
backported, or retain the stock distribution and leave NVIDIA/Linux capture unavailable until a qualified stable
artifact includes them. The custom-build route needs recorded upstream revisions, reviewed backport diffs, new
artifact hashes and a reproducible Chromium/CEF build, followed by actual GPU capture and lifetime qualification.
It would change the dependency artifact, not Miximus's graph, render loop, existing services or GPU queues.

The user explicitly approved the patched-version route on 2026-09-22. Working accelerated capture remains mandatory;
deferring NVIDIA/Linux support is not an accepted completion outcome. The wrapper now contains both upstream patches,
their exact provenance/digests, and an explicit staged source-build tool. Both patches, including Chromium regression
tests, apply to the pinned 152 source. Source acquisition/build is in progress; no patched binary has yet been qualified
or substituted for the stock SDK. See the [source-build instructions](../src/wrapper/cef/README.md).

The full pinned source/dependency sync has now completed. CEF's patch manager applied the Chromium backport,
and repeat generation reported all 116 patches already applied with no failures. Both x64 project configurations
generated successfully. The release SDK compilation has started with official-build optimization/protection defaults,
the pinned PGO profile, and six compilation jobs. The build recipe explicitly bootstraps depot_tools' pinned Python
and prevents its automation from substituting an unpinned `latest` siso package. No application render code changes
are involved. A completed binary, upstream regression-test results and accelerated hardware results are still pending.

The compilation subsequently reached the CEF sample application and failed on its missing GTK headers in the pinned
sysroot. The SDK packaging script does not require that sample binary. The build recipe now requests `libcef`,
`cef_resources` and `chrome_sandbox` directly, preserving the source patches, optimization settings and completed
objects. The resumed build uses ten jobs with matching CPU affinity to bound LLVM's internal worker concurrency.

Checkpoint validation: native build and all 135 non-hardware tests pass. Fresh-profile CEF initialization/shutdown
passes with the system Vulkan loader retained and no validation errors. The real accelerated probe fails at the
documented capture gate; it must not be counted as passing or used to enable a browser node.

## CEF sRGB GPU conversion

Added an explicit `decode_srgb_premultiplied` draw operation. It recovers straight RGB for nonzero alpha, applies
the SDR sRGB transfer function, then premultiplies in linear light; alpha is not gamma-corrected and zero alpha
produces transparent black. The CEF accelerated probe requests this operation for its copy into the ordinary
UNORM16 destination. Existing operation values, defaults, Rec.709 functions and consumers are unchanged. This is an
additive draw operation, not a change to recording, submission, graph evaluation or scheduling.

The controlled DMA-BUF producer test now exercises both raw copying and this conversion with GPU-cleared,
partially transparent source values. The native build, 135 non-hardware tests and all 54 GPU tests pass, including
the five DMA-BUF tests, with synchronization validation. There is no pixel readback; these results establish
execution and lifetime validity, not numerical color accuracy or actual CEF transparency correctness.

The native-handle Chromium backport sets `populates_mappable_shared_image` to false. In the pinned Skia copy path,
the special completion wait for CPU-mappable outputs therefore no longer applies. Actual producer fence publication
must still be qualified before enabling ingress; callback arrival alone must not be treated as GPU completion.
The existing contained DMA-BUF helper exports and waits for published write fences, and retains the borrowed image
until its own GPU copy completes. No CPU pixel transfer or synchronization fallback has been introduced.

The accelerated probe now logs the first incoming DMA-BUF's exported sync-file fence count and status before
copying. This reads synchronization metadata only. Neither a signalled snapshot nor a zero-fence snapshot proves
that all producer writes were published; those observations must be interpreted with the driver and Chromium path.
The pinned Chromium EGL import sources do not select `EGL_IMPORT_EXPLICIT_SYNC_EXT`. The
[EGL implicit-sync control extension](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_implicit_sync_control.txt)
describes implicit synchronization as the default, but that is not evidence that this driver's capture path publishes
the required fences. This diagnostic builds successfully; its actual CEF result awaits the patched binary.

## Patched SDK build and regression tests, 2026-09-23

The patched release library, resources and sandbox now build successfully. All 168 selected upstream
`FrameSinkVideoCapturerTest` cases pass, including the backported native-handle blit cases. Building this suite required
a test-only compatibility patch: CEF's existing `viz_osr_2575` patch exposes `CreateLayeredWindowUpdater` on every
platform, while Chromium's `MockDisplayClient` still guarded that method with `IS_WIN`. The separately hashed
`test_patches` entry removes that guard for this mock method; it changes no production code.

The standard-layout release SDK and local acquisition manifest were generated successfully:

```text
Archive: miximus_cef_linux64_native_handle_r1.tar.bz2
SHA-256: ca969b20a34e3669f8fa2caa42fa81060667197b42793d8e5fdb274ccdb9acad
libcef.so SHA-256: 658a2d4b8ad89c6502d1c51c28f3549124a906301b33d6d7958f40b394f0c006
```

These results establish the source build and regression-test checkpoint. Actual accelerated delivery, producer
synchronization and Miximus GPU import remain separate hardware qualification gates.

### Revision 1 hardware results and synchronization correction

Miximus's full native build and fresh-profile runtime probe passed against revision 1. The system Vulkan loader was
retained. At 640×360, 1920×1080 and 3840×2160, the accelerated probe completed 120 GPU copies with no Vulkan validation
errors. HD/UHD capture timestamps advanced from zero to approximately two seconds, consistent with capture-relative
time; this is not a mapping to program PTS or a performance benchmark.

The expanded fence diagnostic found a critical limit: first and last frame exports both reported a signalled fence
with timestamp `656074305` ns while machine uptime exceeded 173,000 seconds. This matches the kernel's boot-time
stub, returned when no applicable reservation fence is available. `detached-driver` / `signaled-timeline` names alone
cannot identify its origin, because the kernel uses those names for signalled fences generally. See the kernel's
[DMA-BUF export implementation](https://github.com/torvalds/linux/blob/v7.0/drivers/dma-buf/dma-buf.c) and
[stub fence implementation](https://github.com/torvalds/linux/blob/v7.0/drivers/dma-buf/dma-fence.c).
**The successful copies do not establish producer readiness; revision 1 is not synchronization-qualified.**

Revision 2 adds a local dependency patch retaining the existing asynchronous Skia GPU-finished callback for Linux
RGBA blit results. This decouples delivery completion from CPU mappability: the allocation stays GPU-renderable and
non-mappable, while CEF receives the result only after the GPU write finishes. `ReadbackContextTexture::OnMailboxReady`
sends a shared-image result despite its historical name; it performs no CPU pixel readback. The patch affects Linux
RGBA blit requests in this custom binary, not non-blit results, NV12 or other platforms. Miximus still completes its
own GPU copy before returning the borrowed handle. No graph, submission, scheduler or window-service changes are
involved. Revision 2 is building; its regression tests, package and hardware results remain pending.

### Revision 2 qualification checkpoint

Revision 2 built successfully and again passed all 168 selected upstream capture tests. Its archive was verified and
extracted separately, and the staged library digest matches the packaged provenance:

```text
Archive: miximus_cef_linux64_native_handle_r2.tar.bz2
SHA-256: 76314c3e61a412aad1863a63738913b1473c8a25ba0656e7d848a33d29fa4d3a
libcef.so SHA-256: ae6a3d60fed1207ec1e17985f0b1bcc84e022f44c4e9b5320bc4fade16c064f3
```

The full Miximus native build passed against this SDK. A fresh-profile runtime probe initialized and shut down with
the system Vulkan loader retained. HD and UHD accelerated probes each completed 120 GPU copies, browser closure and
runtime shutdown with no Vulkan validation errors. Capture timestamp ranges were `0..2033252` and `0..2049918` µs.
The reservation snapshot still contains the boot-time stub, as expected: producer readiness now follows the retained
Skia GPU-finished callback, not that snapshot. Consumer reads still finish before returning CEF's borrowed handle.

This qualifies the initial GPU-transfer execution path on the local P2000/580.178.04 combination. It does not establish
pixel color accuracy, other adapters/drivers, multi-browser performance, lifecycle stress or completion of the browser
node. Those remain subsequent implementation and hardware-validation work.

## Contained browser session and timed-source queue

Added `detail/browser_session.hpp/.cpp`, still isolated from app-state and node registration. Its construction allocates
one immutable viewport generation and is intended for the subsystem control worker. CEF's UI thread owns browser
callbacks and its independent GPU recording context. The render-thread methods mirror the existing media inputs:
advance, select, resolve, release prepared references and reset. Completed copies enter the existing
`media::timed_source_queue_s` with capture-relative timestamps and separately recorded arrival times, four queued
frames and one nominal frame of playout delay. This is an estimated clock mapping, not request-to-pixel PTS correlation.

Every accepted accelerated callback copies the entire received frame on the GPU and pushes the owned frame into
that buffer. Dirty rectangles are ignored. There is no damage-history processing, missed-draw compensation,
deduplication or content/demand-based capture skipping. This is the permanent capture contract, not an initial
optimization choice; downstream frame selection remains the existing timed-source queue's responsibility.

An eight-slot ordinary texture pool bounds storage for this session generation, with a 1 GiB texture-payload ceiling.
Pool exhaustion drops the incoming paint before importing it. The eventual subsystem must also bound simultaneous
sessions and retiring generations; the per-generation limit is not a process-wide GPU-memory guarantee. Capture stays
hot independently of demand, and the queue retains static frames. Browser closure is asynchronous; shutdown workers
can explicitly await `OnBeforeClose`. Node removal must not wait in the render path. Audio is muted, new browser
windows/downloads and media/permission prompts are denied. Rendered popup composition is not implemented in this
checkpoint and reports an explicit failure rather than publishing an incomplete image.

The wrapper now checks the SDK's source revision, runtime patch list, build arguments and recorded library hash before
allowing real sessions. A same-version stock or revision-1 SDK cannot satisfy that gate. Those SDKs remain available
to the independent diagnostic probes. Configuration tracks binary/provenance changes so replacing an SDK retriggers
verification. An explicit configure check produced capability `0` for revision 1 and `1` for revision 2.

The full native build and all 135 ordinary tests pass. The new manual `cef_session_probe` passed with Vulkan validation
on the P2000: close during pending creation; animated capture and downstream ordinary GPU draws; static retention
(one capture, 60 draws, 59 repeats); exhaustion of all eight leased slots with dropped paints and recovery after
release; a subsequent different viewport; and consumption of an old owned frame after its browser closed. Browser
and runtime shutdown completed without validation errors. This does not yet test in-place resize/navigation, popup
composition, recovery, the app-owned subsystem, native/web node wiring or the command/result bridge.

## App-owned subsystem and Browser node

The app now owns the contained CEF subsystem, with construction and shutdown on the startup thread. Session
allocation and retirement use the existing `utils::serial_executor_s`; no new control queue implementation was added.
Admission allows at most 16 live/pending/retiring requests and 2 GiB of owned texture payload, including generations
still retained by leases or GPU recordings. This excludes Chromium's own memory. A cancelled request withdraws its
published session immediately; the worker observes browser closure, exclusive consumer ownership and pool idleness
before releasing resources. Nodes never wait for that retirement.

Native and web `cef_browser` definitions expose `url`, `size` (one vec2, default `[1920,1080]`, integer pixels bounded
1–4096), `enabled`, and the ordinary `tex` output. CEF's default background is always transparent; there is no
transparency option and no CSS background override. All definitions remain registered in CEF-disabled builds so
saved graphs can load and report unavailable status. Initialization failure also produces unavailable status rather
than breaking unrelated nodes. CEF-enabled startup requests the previously approved optional import capability on
the selected GPU without changing adapter selection.

Capture stays hot independently of graph demand. The integer CEF rate is the ceiling of the program rate, and the
existing timed queue selects frames. URL, viewport or rate changes create a replacement session; the node releases
its old render references before requesting it. Failed sessions retry at most three times with exponential backoff;
configuration changes reset that retry budget. The current replacement behavior restarts the page rather than
resizing a live DOM in place. Reload/CSS controls and their public shape remain deferred.

Full-app testing found that Chrome's browser main parts replace SIGINT/SIGTERM/SIGHUP handlers independently of
`CefSettings::disable_signal_handlers`. The contained runtime saves and restores the host dispositions around
initialization, in addition to setting that CEF flag. No `main.cpp`, graph, scheduler or event-pumping change was made.
The app-state header has one layout regardless of CEF compile definitions; CEF-free consumers must not see a different
object layout.

Validation on the P2000 with Vulkan validation:

- Native build, web build and all 136 ordinary tests passed.
- The pool tests verify that retirement waits for an abandoned/unsubmitted recording as well as frame leases.
- `cef_subsystem_probe` passed bounded admission, pending cancellation, asynchronous removal, a GPU use retained
  after its CPU lease was released, and replacement at a different viewport.
- `scripts/test_cef_browser.py` runs a private settings fixture through the unmodified screen-output node. Animated
  capture, URL/vec2 viewport replacement, static repetition, disable/enable and normal SIGINT shutdown passed with
  no validation errors. The user's settings were not modified.

A separate CEF-disabled native build and all 136 ordinary tests also passed. Popup composition, the internal JSON
command/result bridge, cooperative program-time delivery, additional platform qualification and production
stress/deployment work remain at this checkpoint.

## Internal JavaScript request and JSON-response bridge

Sessions now provide a trusted-native request method accepting a JavaScript function expression and serialized JSON.
It returns a future with serialized JSON or an explicit error. The function executes in the main frame's V8 context
and may return a Promise. No callback or mixer-control object is installed on `window`, and no WebSocket command or
public template API was added. Execution acknowledgement still says nothing about which paint contains its effects.

The ordinary Chromium helper now includes the contained renderer-side handler. Requests carry monotonically
increasing IDs, navigation generations and context tokens based on CEF's globally unique frame identifier. Context
creation/release is announced privately. Navigation, renderer termination and closure cancel pending requests; stale
replies cannot settle a request in a replacement context.

Both sides bound pending requests to 64; function source is limited to 64 KiB and JSON payload/results to 1 MiB.
Timeouts are positive and at most 30 seconds, checked by one weakly owned timer per session. A timed-out command's
future settles when the UI-thread timer observes its deadline, but its in-flight capacity remains reserved until the renderer acknowledges cancellation
or returns a result. This prevents repeated timeouts from building an unbounded IPC backlog behind a hung renderer.
Cancellation does not undo JavaScript side effects or forcibly interrupt JavaScript already running.

The accelerated subsystem probe passed correlated/out-of-order JSON and Promise replies, thrown/rejected errors,
undefined/circular results, invalid request JSON, 64-command saturation, timeout/cancellation acknowledgement and
capacity recovery, navigation to a new main-frame context, and closure cancellation. Existing GPU retirement and
subsystem shutdown checks still passed with Vulkan validation enabled. The full native build passed; no CPU pixel
path or graph/render-loop changes were introduced.
