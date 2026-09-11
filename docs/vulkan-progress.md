# Vulkan implementation status

The application uses Vulkan directly through `src/gpu/` and `src/gpu/detail/`. OpenGL wrappers and DVP implementation
code have been removed. Vulkan headers come from the SDK; Volk and VMA are pinned submodules integrated through
`src/wrapper/vulkan/`. GLSL sources live in `shaders/`; the existing file bundler embeds only validated SPIR-V.

## Current behavior

- Working images retain the linear, premultiplied UNORM16 color contract. Drawing and conversion APIs use explicit
  blend, compositing, transfer-function and alpha-mode enums. See [GPU and media](gpu-and-media.md).
- Vulkan staging is the default transfer backend. Linux CUDA/Vulkan transfers require `--use-cuda` and refuse fallback.
  The [CUDA guide](cuda-transfers.md) includes repeatable verification and cross-machine benchmarking commands.
- DeckLink retains direct SDK access to backend-owned host allocations and external leases. Future DVP integration
  must preserve the [direct-memory contract](decklink-direct-memory.md).
- NDI input and output independently support ignored, straight and premultiplied alpha. See [NDI alpha modes](ndi-alpha-modes.md).
- GLFW owns windows and monitor discovery. Linux uses X11/XWayland to preserve saved pixel sizes and desktop positions;
  the Xrandr dependency remains in `src/wrapper/x11/`. Screen presentation uses a FIFO worker with presentation-ID completion feedback when supported, and an explicitly labelled nominal FIFO estimate otherwise.
- Independent recording contexts separate graph, transfer, and presenter command pools. A submission worker resolves
  initial image layouts and submits commands. The render thread does not acquire a global recording/queue lock.
- NDI/DeckLink producers start uploads before publishing into the timed FIFO. Rendering still waits for the exact
  PTS-selected upload at consumption after source buffering. FIFO-owned leases retain later completed uploads.
- The executor owns pending output publication through a RAII frame scope. Failed evaluations release unpublished
  leases; successful native submission publishes them off the render thread. `complete()` retains its CPU cleanup role.
- Descriptor pools grow in reusable pages instead of imposing a fixed draw count. In-flight recording counts remain
  bounded; exhausted recording capacity or descriptor memory drops the unfinished evaluation safely.

All pipelines are prepared before frame processing. An application-managed disk cache was removed because its measured
saving with a warm NVIDIA driver cache was about 8 ms once at startup (9.75 ms without app persistence versus 2.02 ms
with it). With driver caching disabled, the corresponding measurements were 116.98 ms and 5.21 ms. These are medians of
ten alternating process runs on a Quadro P2000, driver 580.178.04, and exclude the rest of device startup. Compiling and
validating the seven shaders at build time took 132 ms in parallel or 631 ms sequentially, medians of five batches.
These results justify the local simplification, not a performance claim for other drivers.

## Build and verification

Use the [development guide](development.md) for normal, dedicated tidy and sanitizer builds. `MIXIMUS_VULKAN_DEVICE_UUID`
selects a device, with optional dashes; `MIXIMUS_VULKAN_VALIDATION=1` requires the installed Khronos synchronization
validation layer. Missing requested validation or an unsupported device floor is an error.

Use the Khronos layer from **Vulkan SDK 1.4.357.0** for validation. The previously used 1.4.341 layer has a
concurrency bug in its own submission/presentation history. An ASan-instrumented build of that layer reproduced a
heap-use-after-free: `PreCallValidateQueuePresentKHR` imported a `BatchAccessLog` entry while the submission thread
freed it in `BatchAccessLog::Trim`. The layer's
[upstream QueuePresent fix](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/196b5e2b1fa1)
protects this state against concurrent queue validation. This is independent of CUDA memory ownership; CUDA traffic
made the race observable in the hardware graph. Concurrent operations on different queues remain part of the app's
design.

The fix is to update the optional validation tool. Application queue ownership, CUDA transfers, upload readiness and
sanitizer defaults are unchanged. Do not disable synchronization validation or set
`VK_LAYER_FINE_GRAINED_LOCKING=false` to obtain a passing result. The normal application does not load this optional
layer unless requested. The project's Vulkan headers, Volk, VMA and shader compiler do not need updating for this
validation-tool correction.

The tested layer and its manifest are installed locally under the ignored build directory. From the repository root:

```sh
export VK_LAYER_PATH="$PWD/build/tools/vulkan-validation/1.4.357.0/x86_64/share/vulkan/explicit_layer.d"
export MIXIMUS_VULKAN_VALIDATION=1
unset VK_LAYER_FINE_GRAINED_LOCKING
```

On another machine, use the corresponding manifest directory in an installed
[Vulkan SDK 1.4.357.0](https://sdk.lunarg.com/sdk/download/1.4.357.0/linux/vulkansdk-linux-x86_64-1.4.357.0.tar.xz).
The Linux archive's SHA-256 is `0f09bf6a0625e346bf004be70b92907e934a4c76606b323441b2baf3a5a0e66d`.
Only `x86_64/lib/libVkLayer_khronos_validation.so` and
`x86_64/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json` are needed for these checks; retain their SDK
directory structure so the manifest resolves its library. No external implementation is copied into the source tree.

Ordinary CTest is GPU-independent. Run hardware regressions explicitly on a suitable GPU/display:

```sh
build/src/gpu/gpu_vulkan_test
build/src/gpu/gpu_transfer_vulkan_test
./scripts/test_cuda_transfers.sh build 3
build/src/gpu/gpu_window_test
```

Set `MIXIMUS_TEST_VULKAN=ON` when configuring to register hardware suites with CTest. The transfer benchmark is an
explicit build target; migration-only storage/DMA probes and their separate build mode have been removed.

The 2026-09-10 ownership changes passed the normal build, a dedicated Clang 21 tidy build with zero diagnostics,
106 ordinary tests, 27 renderer tests, ten Vulkan transfer tests, ten CUDA transfer tests, and four window tests on the
Quadro P2000. GPU suites used Khronos synchronization validation. ASan/UBSan with leak detection also passed all 106
ordinary tests and the renderer/transfer suites in both backend modes, preserving the existing CUDA sanitizer defaults.
The maintained transfer benchmark built and verified pixels with both backends; its short validated runs were
correctness checks, not comparative performance measurements.

The copied DeckLink Duo (2) input / Duo (1) output, local NDI loopback, and screen graph ran for 30 seconds with each
backend after builds finished. Both runs completed shutdown with no validation errors, transfer failures, or GPU
recording-capacity drops. DeckLink output reported no dropped frames; NDI reported no receiver video drops. One Vulkan
NDI render-target acquisition missed; both screen runs still had a few timing/queue drops. The original saved settings
were unchanged. A further 15-second CUDA hardware run passed ASan/UBSan, leak detection with the existing driver
suppressions, and synchronization validation through complete shutdown. These runs establish working transfers and
handoffs, not perfect or long-duration display cadence.


The screen-cadence follow-up restores the OpenGL X11 nominal monitor deadlines and unthrottled presentation contract
(immediate, with mailbox/FIFO capability fallback). Timestamp selection now precedes the presenter worker's wait for
that exact frame; acquisition is no longer fed into the clock estimator. Missed monitor periods advance the clock by
all elapsed intervals. Short screen/readback metadata-lock contention no longer counts as exhausted render capacity.
That follow-up retained a fixed preroll offset. The subsequent behavior review rejected that change: NDI and screen
now continuously observe newly selected frame PTS again, as before the rewrite. DeckLink corrective observations no
longer require a full queue. Screen retains its original full-queue condition for the separate fallback correction,
as explicitly confirmed by the user.

This follow-up passed the normal build, a dedicated tidy build with zero diagnostics, 110 ordinary tests, five display
tests, and ten transfer tests with each of Vulkan and CUDA. Display/transfer checks used synchronization validation;
the new display test deliberately delays the selected frame and verifies that the worker does not replace it.
Two further 30-second copied hardware-graph runs, one per transfer backend, completed shutdown without GPU validation
errors or transfer failures. After the first five seconds of status collection, both screens recorded zero timing drops,
queue overflows, skipped intervals, or render-slot misses; each repeated one frame while converting 60000/1001 program
frames to the nominal 60 Hz monitor cadence. Startup still incurred three/one queue overflows and two/one skipped
intervals for Vulkan/CUDA respectively. Saved settings were unchanged. The user continued to observe stuttering after these runs. These counters therefore do not establish restored visual
cadence; physical scanout, prolonged occlusion, and native Wayland presentation still require separate observation.


The [behavior review](vulkan-behavior-review.md) records the subsequent restorations and the reasons for retained
implementation differences. Earlier test results above predate those restorations.

## Open work

The following review findings remain work, not completed acceptance claims:

- Validate prolonged occlusion, native Wayland/FIFO pacing, mixed display refresh rates, and long-term drift separately
  from the restored X11 nominal cadence. Completion boundaries and nominal refresh status are not physical scanout
  measurements.
- Align device eligibility with actual surface, swapchain-maintenance and conversion requirements. Test-only formats
  no longer gate eligibility; unsupported output capabilities still need targeted multi-device validation.
- Extend failure coverage beyond the recoverable CUDA submission errors exercised by the behavior review to device loss,
  permanently stalled DMA and other backend failure points.
- On devices with only one queue, a slow present call can still delay the submission worker. The render thread no
  longer takes that mutex, but sustained driver delay can exhaust its bounded in-flight capacity.
- Validate Windows and macOS/MoltenVK builds and hardware behavior. Linux cross-configuration did not establish either.
  Exercise noncoherent memory, physical monitor unplug, surface/device loss, long occlusion and other GPUs.
- Investigate the unsuppressed XCB/Xlib/GLFW window-suite leaks described in [GPU sanitizers](gpu-sanitizers.md).
- Native-precision NDI planar formats and future FFmpeg hardware decoding remain separate format/feature work.

## Screen timing correction awaiting visual confirmation

The desired-result review identified four coupled problems: source-PTS quantization fed back as accumulating latency,
CPU submission/copy times used as display observations, a CPU producer wait after the intended presentation deadline,
and newest-eligible selection instead of nearest PTS. The corrected screen path uses FIFO plus `VK_KHR_present_wait`
where supported, submits the producer timeline dependency as a GPU wait, and selects the nearest retained/queued PTS.
It recovers display phase separately from program PTS and slews continuous scheduling-error corrections so notification
jitter does not produce repeated cadence transitions. This supersedes the earlier nominal X11/immediate restoration.
The original full-queue fallback remains; NDI/DeckLink selection and input-upload consumption are unchanged by this fix.

The NVIDIA display supports present-wait but not `VK_GOOGLE_display_timing`; status explicitly reports a display
completion estimate, not an exact scanout timestamp. The normal build passes 125 CTests, including live bounded-queue
simulations with continuous feedback, rational cadence mismatch and notification jitter. All six window tests pass,
also with Vulkan synchronization validation. Tidy and sanitizer builds are deferred until the user confirms the main
fix visually. See [screen presentation](gpu-and-media.md#screen-presentation) for the current contract.

The final normal-build copied hardware graph ran for 90 seconds with each transfer backend and shut down cleanly.
After the first ten seconds, the Vulkan run had zero timing drops, skipped display intervals, queue overflows or slot
acquisition misses, with four cadence repeats. The CUDA run had one skipped display interval and one timing drop,
five repeats, and no further queue overflows or slot acquisition misses. Startup still included frame skips and one
CUDA-run queue overflow. These counters do not replace the requested visual confirmation of animation smoothness.
No changes were staged, and the runs used temporary settings copies.
