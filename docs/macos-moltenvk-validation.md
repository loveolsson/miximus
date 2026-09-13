# macOS / MoltenVK hardware validation

Status: **not tested on Mac hardware**. Source review dated 2026-09-13, against the Miximus Vulkan backend and
MoltenVK **1.4.1**, the package version currently checked by the Vulkan wrapper. This document records concerns and
experiments, not confirmed runtime failures or measured performance results. CUDA is outside this review's scope.

Companion documents: [GPU and media](gpu-and-media.md), [Vulkan validation plan](vulkan-validation.md), and
[implementation status](vulkan-progress.md). Recheck the cited implementation when testing a different revision or
MoltenVK release. Proposed experiments below may require instrumentation or code changes; they are not existing
runtime switches.

## Test environment and initial checks

Start with available Apple Silicon hardware. Treat Intel/AMD Macs as separate configurations when available; do not
generalize Apple Silicon memory or tile-rendering results to them.

Record:

- Miximus commit, build type, compiler, architecture (native arm64 versus Rosetta), macOS version, Mac model, GPU,
  and RAM.
- Vulkan SDK, loader, **actually loaded** MoltenVK library/version, GLFW, shader compiler, and validation-layer
  versions. Retain device diagnostics: features, formats, queues, memory types, and enabled extensions.
- Display model, connection, logical and framebuffer dimensions, content scale, refresh mode, fullscreen/windowed
  state, and variable-refresh configuration where applicable.
- Graph/settings snapshot, resolutions, frame rates, buffer depths, stream counts, and any attached media hardware
  and SDK/driver versions.

With a configured build that enables `BUILD_TESTING`, run from the repository root:

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
MIXIMUS_VULKAN_VALIDATION=1 build/src/gpu/gpu_vulkan_test
MIXIMUS_VULKAN_VALIDATION=1 build/src/gpu/gpu_transfer_vulkan_test
```

The GPU commands require an installed, discoverable Khronos validation layer. Save their complete output and
distinguish unsupported optional formats from failures of required application paths. These tests do not establish
working screen output or real SDK DMA. `gpu_window_test` is currently built only on Linux; adapting its geometry and
presentation checks for macOS is follow-up work, not a completed test.

Use Metal validation/frame capture to investigate correctness, then run separate release measurements without
validation/capture overhead. Measure usable end-to-end frame latency as well as GPU duration. Compare identical
quality, cadence, and buffering; preserve raw results and use repeated runs after warmup. The broader validation plan
defines workload and endurance methodology.

## 1. Display-clock feedback may observe GPU completion instead of presentation

**Priority: first correctness investigation.**

Miximus's [`complete_presentation()`](../src/gpu/presenter.cpp) timestamps the return from `vkWaitForPresentKHR`.
[`presentation_complete()`](../src/nodes/screen/detail/output_presenter.cpp) feeds that observation into the display
clock, program-to-display mapping, and latency metrics. Present-wait support also selects display pacing instead of
the nominal application pacing wait.

In [MoltenVK 1.4.1's image implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm#L1482),
the Metal command-buffer completion handler notifies presentation-ID completion separately from the drawable's
presentation callback. The [swapchain wait](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/MoltenVK/MoltenVK/GPUObjects/MVKSwapchain.mm#L222)
observes that ID. The inferred risk is early or irregular display-clock feedback even with correct image rendering.
Do not assume that extension availability proves the timestamp represents a display boundary.

Test:

- Instrument producer PTS, selected frame, submission, present-wait return, and actual presentation observations.
  Compare with drawable presentation timestamps or `VK_GOOGLE_display_timing` where usable; calibrate clock domains
  before subtracting timestamps.
- Exercise 60 Hz, 60000/1001 content, higher/variable refresh where available, and multiple displays. Use visible
  frame numbers or a moving cadence pattern to catch repeats and skips that counters can miss.
- Compare the current clock with an experimental nominal-pacing path and, if available, a path using actual
  presentation feedback. Exercise resize, minimize, occlusion, fullscreen changes, display disconnect, and shutdown.

Record p95/p99/max timing error, visible cadence, repeats/drops, and estimated versus observed latency. Accept the
clock only when its observation semantics are understood and frame selection remains stable; otherwise classify
the feedback as an estimate and correct its pacing use. Keep presentation semaphore retirement distinct from timing.

## 2. One render pass per draw may be expensive on Apple Silicon

**Priority: first rendering-performance investigation.**

[`recording_state_s::draw()`](../src/gpu/detail/drawing.cpp) opens and closes dynamic rendering for every draw,
using attachment `LOAD` and `STORE` over the target extent, including replacement draws. Apple Silicon's tile-based
rendering can benefit from retaining attachment data within a pass; repeated pass boundaries may add bandwidth and
encoder overhead. UNORM16 working targets contain eight bytes per pixel.
[Apple's rendering guidance](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering)
explains the architectural motivation; the magnitude for Miximus remains unmeasured.

Test identical HD/UHD graphs with increasing layer counts, partial overlays, source-over blending, and full-target
replacement. Capture Metal render-pass/encoder counts, attachment traffic where available, GPU time, and frame
deadline misses. Compare an experimental implementation that groups compatible draws to the same target and omits
loads only when a full overwrite makes them unnecessary. Preserve ordering, untouched pixels, and premultiplied
alpha. Accept an optimization only if output matches and sustained capacity or tail latency improves beyond noise.

## 3. Broad barriers and mipmap work may limit throughput

[`recording.cpp`](../src/gpu/recording.cpp) uses broad `ALL_COMMANDS` source dependencies, emits transitions for repeated
uses even with the same layout, and generates mip levels with individual blits and barriers. The staging upload path
explicitly requests mip generation; conversion and graph boundaries also generate chains. Some consumers never
minify, so producer-side generation may do unused work.

Compare one-to-one, magnifying, and minifying graphs with shared sources and repeated target writes. Record barriers,
blits, Metal encoder boundaries, and GPU time. Experiment with tracking precise previous access and avoiding redundant
dependencies, or delaying unused mip generation. An unchanged layout alone does not make a barrier redundant.
Verify minification quality and dirty-chain regeneration after slot reuse and abandoned recordings. No optimization
may weaken submission-order layout resolution or expose stale mip levels.

## 4. Staging copies may leave shared-memory performance unused

[`vulkan_staging_s`](../src/gpu/transfer/detail/transfer_backend.cpp) copies packed frames between a mapped host buffer
and a separate device buffer; image transfers also use staging copies. On Apple Silicon, directly consuming or
producing suitable shared buffers could remove a full-frame copy, especially for v210. Shared access is a candidate
to benchmark, not an assumed winner over private device storage.

Compare current staging with an experimental shared-buffer conversion path for simultaneous upload and readback,
real SDK strides/alignment, repeated frame sampling, and multiple HD/UHD streams. Measure producer-ready to usable
texture and render-ready to readable output, CPU cost, GPU copy/conversion time, and retained memory.

Run pixel/padding comparisons and real DeckLink/NDI operation where hardware is available. Preserve SDK access to the
backend-owned stable address, exact upload selection, completion-before-readback, cache maintenance, and all GPU/SDK
leases. Synthetic GPU throughput alone cannot qualify a DMA allocation or justify replacing the transfer backend.

## 5. Blit presentation disables framebuffer-only Metal drawables

[`presenter.cpp`](../src/gpu/presenter.cpp) creates a transfer-destination swapchain and presents by linear blit.
[MoltenVK's swapchain implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/MoltenVK/MoltenVK/GPUObjects/MVKSwapchain.mm#L455)
sets `CAMetalLayer.framebufferOnly` to false for that usage. This may give up a Metal optimization; it is not evidence
that blit presentation is unsupported.

Compare the current path against an experimental final graphics draw into a color-attachment-only swapchain. Measure
GPU duration, display latency, and sustained multi-output performance at native and scaled sizes. Check orientation,
filtering, alpha, and linear-to-sRGB encoding with gradients and asymmetric markers. Retain correct acquire/present
semaphore ownership. Document that the existing output selects 8-bit sRGB; wide-gamut/HDR output is separate work.

## 6. Retina scaling may change requested pixel geometry

[`window.cpp`](../src/gpu/window.cpp) disables `GLFW_SCALE_TO_MONITOR` but does not disable
`GLFW_SCALE_FRAMEBUFFER`. On macOS, logical window dimensions and framebuffer pixels can differ. A 2x framebuffer
scale gives four times the pixel count. The code does track framebuffer dimensions, so the concern is intended
output geometry, resampling, and workload rather than a known viewport bug.
[GLFW's scaling documentation](https://www.glfw.org/docs/3.4/window_guide.html#window_scale)
describes the distinction.

Test saved window sizes, fullscreen, and movement between Retina and non-Retina displays. Record requested logical
size, actual framebuffer size, swapchain extent, and source/output resolution. Check a one-pixel grid for unintended
resampling. Establish whether the screen node promises logical dimensions or physical pixels before changing hints.
Adapt Linux window tests instead of carrying over their assumption that the two sizes are identical.

## 7. Runtime discovery, feature requirements, and startup need Mac qualification

The [Vulkan wrapper](../src/wrapper/vulkan/CMakeLists.txt) checks MoltenVK headers for version 1.4.1, while Volk loads
the runtime dynamically. A successful header check does not prove which runtime is loaded. The device requires
Vulkan 1.3, dynamic rendering, synchronization2, and timeline semaphores; the presenter additionally requires
swapchain maintenance. MoltenVK 1.4.1 documents the relevant support, but older or mismatched installations can fail.

Test a development launch and the intended distribution on a Mac without the SDK or SDK shell environment. Verify
loader/ICD or framework discovery, embedded libraries, signing where applicable, and the loaded driver identity.
Unsupported configurations should fail with actionable diagnostics rather than appearing to start with missing output.

[`initialize_pipelines()`](../src/gpu/detail/pipeline.cpp) warms the pipelines at startup with a null Vulkan pipeline
cache. Measure cold and warm startup separately from steady-state rendering. If conversion/compilation is material,
compare an application pipeline cache; its previous Linux measurements do not settle the Mac decision.
[MoltenVK's runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/Docs/MoltenVK_Runtime_UserGuide.md#shader_load_time)
describes caching SPIR-V-to-MSL conversion results.

## 8. Verify format and shader behavior without presuming incompatibility

Portability enumeration and the device portability extension are already handled in [`device.cpp`](../src/gpu/device.cpp).
The graphics path uses triangle lists, identity image views, small descriptor sets, and 128-byte push constants.
It does not depend on geometry shaders, Vulkan events, or optional image-view swizzling.

UNORM16 is not inherently a blocker:
[MoltenVK's format table](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/MoltenVK/MoltenVK/GPUObjects/MVKPixelFormats.mm#L1038)
includes `RGBA16Unorm`. Still qualify the actual combined image usages, filtering, blending, mip blits, and storage
access on the selected device. Buffer conversion is optional during device selection but throws when used without
support; v210 paths must be exercised even if basic rendering starts successfully.

Run the renderer/transfer pixel tests, including padded RGBA/BGRA/BGRX/ARGB and v210, partial six-pixel groups,
legal-range boundaries, alpha edges, mixes, and minification. Compare intermediate stages against independent CPU
references, with explicit tolerances for color arithmetic. SPIR-V validation alone does not test translated MSL
execution. Preserve the UNORM16 color contract unless a separate, measured design change justifies replacing it.

## Results to retain

For each numbered concern, append or link a report containing:

| Field | Required evidence |
| --- | --- |
| Status | Not tested, validated for the named configuration, failed, or unsupported |
| Reproduction | Source revision, environment, graph/settings, commands, duration, and candidate changes |
| Correctness | Pixel comparisons, timing observations, validation logs, lifecycle and SDK results |
| Performance | Repeated median/p95/p99/max results, drops/repeats, memory, and relevant Metal captures |
| Decision | Supported conclusion, remaining uncertainty, and any implementation follow-up |

Prioritize display-clock semantics and basic renderer/transfer correctness before optimizing bandwidth. No entry is
validated until exercised on the recorded Mac hardware; missing displays or media devices remain explicit gaps.
