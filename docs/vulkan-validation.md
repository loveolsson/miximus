# Vulkan migration validation and performance gates

Status: proposed; 2026-09-08. Companion to the [migration plan](vulkan-migration.md) and
[transfer strategy](vulkan-transfers.md). This is a test plan, not a report of completed hardware validation.

## Stage 0: local Linux probes

Build a small native probe/benchmark target using the real transfer formats and SDK allocator contracts. It should
produce a machine-readable capability/result record plus a short human-readable report. Keep shader/compiler setup
and small GPU primitives reusable by the implementation, but avoid turning the probe into another media framework.
Run the relevant probes on this Linux machine under native Wayland. Defer Apple/Windows-specific experiments to
their platform passes; unavailable hardware must not delay the initial rewrite.

Performance experiments are required during these probes and the vertical slice. Compare buffer versus image storage,
direct conversion versus copying, memory choices, and queue/submission topology under concurrent work before broad
node integration makes those decisions expensive to change. Record which architectural choices the results support,
which they contradict, and which remain untested. Continue measuring through cutover and subsequent tuning.

This Quadro system is a single sample, not representative evidence for Linux or NVIDIA as a whole. Every benchmark
conclusion must name its tested configuration. Separate API/correctness constraints from local performance rankings;
leave predictions for other GPU families, drivers, or memory architectures explicitly unverified. Other hardware need
not be available now, but local winners must not become unconditional vendor/OS-wide policy.

| Probe | Concrete experiment | Decision produced |
| --- | --- | --- |
| Platform floor | Enumerate loader/device/MoltenVK versions, queue families, required features, memory types, working/transfer formats, presentation and interop capabilities. Compile/run the basic shaders. | Pin SDK/MoltenVK/toolchain versions and supported OS/GPU floor. Record unsupported devices explicitly. |
| DeckLink allocation | Capture and schedule output with persistent Vulkan mappings; separately test controlled aligned host allocations imported into Vulkan. Exercise allocator object reuse and SDK DMA. | Which storage satisfies the SDK without another CPU copy on each tested platform? |
| NVIDIA bridge | Where supported locally, run both directions through Vulkan-exported buffers and CUDA. | Does CUDA beat native Vulkan under the real concurrent workload? DVP is excluded. |
| UMA conversion (later Mac pass) | v210/RGBA upload and output using shared buffers, image copies, and device-private alternatives on Apple Silicon. | Does direct buffer conversion remove work in the complete path? |
| Display isolation | Render an offscreen workload while acquiring/presenting to minimized, resized, occluded, disconnected, and mixed-refresh windows. Exercise available queue topologies. | Can display stalls remain outside program rendering on each window system? |
| Color/storage | Compare current UNORM results with candidate four-channel targets; validate blending, packing, filtering, and display encoding. | Choose working storage and explicit numerical tolerances before broad migration. |

These probes require actual hardware. This research session did not execute them. A successful Vulkan import in a
synthetic test is not enough to accept a DeckLink DMA allocation or a decoder surface.

## Hardware and build coverage

Validation follows the implementation order:

1. **Now: this Linux machine, native Wayland.** Inventory the installed GPU, driver, SDKs, and available I/O hardware.
   Benchmark current working non-DVP transfer paths and the Vulkan implementation locally. X11/XWayland may be used
   for the old-backend reference if necessary; record that presentation environments differ.
2. **Initial portability attempt: Mac and Windows without hardware.** Implement the platform paths from the documented
   APIs and shared design. Run compile/cross-compile/static checks where the toolchains permit; state explicitly when
   a build or runtime check could not be performed. Missing toolchains or runtime failures on later machines do not
   invalidate the Linux milestone.
3. **Next: Mac hardware.** Fix and benchmark MoltenVK, shared-memory transfers, display output, and available media I/O.
4. **Then: Windows hardware.** Fix and benchmark native Vulkan, relevant vendor interop, display output, and media I/O.

Additional NVIDIA/AMD/Intel, integrated/discrete, and Intel Mac coverage is later hardening as machines become available,
not a mandatory matrix before Linux cutover. Limit validation claims to the machines actually tested. DVP is not built
or tested in the new implementation.

Record GPU identity, driver, CPU/NUMA layout, RAM, PCIe link/topology, OS/window system, monitor modes, DeckLink model/
driver, NDI version, compiler/build mode, Vulkan SDK, MoltenVK, and enabled extensions. Test packaging on machines without
the development SDK installed. Windows loader/optional DLL behavior and macOS library/framework embedding/signing are
checked during their respective platform passes; they do not block Linux cutover.

Use the old revision on the same machine for OpenGL comparisons where it runs. On macOS, establish absolute capacity
and latency results because the existing GL 4.6/CUDA-dependent implementation is not a valid universal baseline.
Missing hardware remains a recorded follow-up, not a simulated pass or a blocker for the current platform.

## Correctness before performance

Automate backend tests that expose actual failure modes:

- GPU round trips for RGBA, BGRA, BGRX, ARGB and v210, with padded strides, alignment variations, tiny/odd dimensions,
  partial groups, and guarded padding. Packing tests compare bytes against CPU references; color tests use explicit
  tolerances and independently calculated range/matrix expectations.
- Color bars, black/white and legal-range boundaries, near-transparent premultiplied edges, gradients, keyed output,
  Rec.601/709/2020 modes, minification/mips, crops, and asymmetric orientation markers. Compare stage outputs so an
  encoding error cannot be concealed by an inverse error later in the pipeline.
- All existing compositing operations, two-input mixes, graph fan-out, ordered mutable framebuffer chains, missing
  inputs/private fallback targets, and repeated source frames. Test multi-frame reuse while GPU work remains pending.
- Producer writes, GPU reads/writes, CPU readback, and SDK ownership must never overlap illegally. Test noncoherent
  memory when available, atom-aligned suballocations, aborted recordings, and multiple simultaneous consumers.
- Exact source selection: delay the selected upload and prove the output does not silently use an older ready frame.
  Conservative submission that never executes must release ownership through its normal bounded queue.
- Remove/reconfigure nodes during DMA, upload, rendering, readback, asynchronous NDI sends, and scheduled DeckLink
  playback. Reuse custom allocator objects across many access cycles. Force pool exhaustion, import failure, allocation
  failure, cancellation, source epoch changes, and device-loss/error handling.
- Resize/recreate swapchains with work outstanding; verify binary semaphore reuse and distinguish image render
  completion from presentation-resource retirement.

Retain and run existing scheduler/source/output timing tests. The Vulkan migration must not replace the media clock,
cadence policy, or graph scheduler. Extend their fake completion primitives to cover the new submission model.

Use Vulkan validation including synchronization checks, then GPU-assisted validation where supported. Use Metal
validation/frame capture for Apple-specific failures. Run correctness and sanitizer builds separately from throughput
measurement. No unexplained validation errors, host use-after-free, or indefinite completion waits may remain.
[Khronos synchronization validation guidance](https://docs.vulkan.org/guide/latest/synchronization.html)

## Performance methodology

For each endpoint, compare only correct candidates at identical format, quality, queue depth, and frame rate. Separate
cold allocation/registration/pipeline costs from steady state. Reconfigure repeatedly as a separate latency test.

Test representative SD/HD/UHD dimensions, 50/60 and 1000/1001 rates, actual SDK stride/alignment, alpha modes, and small
text updates. Add larger modes only where hardware and product requirements make them relevant. Scale concurrent
streams until the deadline/capacity limit is reached; one 1080p stream is not sufficient evidence.

Measure at least:

| Measurement | Why it matters |
| --- | --- |
| Producer-ready → sampled/converted input and render → host-ready output | Measures usable latency, including queues and conversion. |
| Output target → actual SDK/display observations, with observation type recorded | Detects cadence regressions and distinguishes estimates from hardware timestamps. |
| Median/p95/p99/max transfer and render timing; missed deadlines/repeat/drop counts | Averages can hide periodic pipeline stalls. |
| CPU time, memcpy bytes, GPU copy/conversion time, submissions and handoffs | Explains a result rather than merely naming a backend. |
| Sustained simultaneous input/output capacity | Finds copy-engine contention, SDK CPU work, memory bandwidth, and scheduling bottlenecks. |
| Retained host/device memory, SDK frames, and deferred resources | Detects apparently fast paths that consume excessive buffering or prevent pool reuse. |

Use CPU steady-clock timestamps for end-to-end latency. GPU timestamps characterize GPU intervals; correlate clock
domains only with a supported calibrated mechanism. Never subtract timestamps from unrelated clock domains directly.
Repeat steady-state runs, record spread, and alternate candidate order to limit thermal/load bias. A candidate wins
only beyond measurement noise; otherwise prefer simpler ownership and fewer dependencies.

Proposed steady-state run length: at least five minutes per candidate/workload after warmup, with multiple runs for
close results. Then exercise a combined graph: multiple captures, text/teleprompter, mixes/minification, SDI and NDI
outputs, and screen presentation. Include receive/send loopback and independent sources so shared clocks do not mask
cadence problems. Stress network jitter, CPU load, and compositor stalls separately.

Do not rank host import, CUDA, or shared-memory sampling from standalone GB/s. Include registration amortization,
CPU cache behavior, SDK buffer retention, conversions, repeat use, and interference with rendering. Test dedicated
transfer queues against graphics-queue copies; account for ownership barriers and synchronization overhead.

## Acceptance and recorded outcomes

The initial Linux cutover needs correct rendering/media ownership, bounded memory, working native Wayland output,
and useful local benchmark results. Performance does not need to be perfect on the first try. Record regressions
against the working non-DVP baseline and prioritize them in the Linux tuning pass; intermediate regressions do not
require a new approval before continuing implementation and optimization.

For the Linux tuning milestone, target no meaningful loss in stream capacity or p99 usable-frame latency at equivalent
quality/buffering on this machine. Use repeated measurements and actual workloads to judge the result. Report remaining
regressions and unsupported behavior candidly. Mac and Windows establish their own workload and performance targets
when their hardware passes begin; do not apply Linux rankings or timings as if they were portable measurements.

For each platform pass:

1. Validate current-node functionality, color/timing behavior, and lifecycle correctness on the available machine.
2. Test enabled fast paths and fallback behavior; distinguish measured choices from uncalibrated functional defaults.
3. Measure concurrent rendering/transfers and retained memory. Fix sustained queue growth and unsafe reuse immediately.
4. Run short repeatable stress/teardown tests during development. Progress to a 24-hour mixed-I/O soak and multi-day SDI
   endurance testing during later hardening; those long runs do not block the initial Linux rewrite or portability attempt.
5. Test display/compositor stall containment; keep ordinary frame-loop global device-idle waits, synchronous pipeline
   compilation, and SDK control calls out of the design.
6. At Linux cutover remove the GL renderer/transfers, GLAD/GLX dependencies, and all DVP integration. Keep NVIDIA
   dependencies optional. Ordinary node headers remain free of graphics/platform API types.

Store reports with source revision, configuration, candidate settings, raw results, and interpretation. Each platform/
endpoint entry should say **validated**, **unsupported**, **failed**, or **not tested**, with its fallback and reason.
Attach the final results to the migration review. Future codec support has its own profile/format and seek/loop/endurance
gates and must not be claimed complete merely because the compositor accepts imported images.
