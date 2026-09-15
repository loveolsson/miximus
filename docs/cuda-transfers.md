# CUDA/Vulkan transfers and verification

Vulkan staging is the default. Pass `--use-cuda` to request CUDA transfers on Linux builds with CUDA support.
CUDA is enabled only when the selected Vulkan GPU has a matching,
usable CUDA device and the required external-memory/semaphore extensions. Before selecting CUDA, startup creates and
imports every required host-format/sampling combination through the production allocator and CUDA backend, for uploads
and readbacks. This covers raw RGBA8 images with and without mipmaps, padded v210 word buffers, aligned pinned host
storage and external semaphores. Probe resources retire through RAII without submitting GPU work.

If any resource cannot be created/exported/imported, all probe resources are released and **CUDA is disabled for the
entire run**, before any stream starts. One startup warning lists the missing capabilities and failed operations; duplicate failures for channel-order aliases
are reported once. The same list is available as `cuda_missing_support` in device diagnostics. A usable CUDA runtime alone is not sufficient. Missing CUDA support likewise selects Vulkan staging.
This is format/resource qualification, not a guarantee against later memory exhaustion, device loss or driver faults.

Without `--use-cuda`, startup skips CUDA probing and initialization entirely. `--disable-cuda` is removed. Selection is immutable for the device lifetime: once CUDA is chosen, allocation, format or
transfer failures are errors, never reasons for a per-stream staging fallback. The former
`MIXIMUS_GPU_TRANSFER_BACKEND` environment variable remains ignored.

Startup uses the existing `gpu` component logger. A device without CUDA support logs at WARN:

```text
CUDA transfers disabled: CUDA is not supported on this device
```

Missing build/interoperability support, runtime errors or failed resource qualification produce one WARNING with details:

```text
CUDA transfers disabled due to missing support:
   --- RGBA8 image with mipmaps: map CUDA raw RGBA8 frame image: operation not supported
   --- v210 word buffer: map CUDA packed frame buffer: operation not supported
```

Omitting `--use-cuda` logs `CUDA transfers disabled: --use-cuda was not specified` at WARN. Successful qualification logs `CUDA transfers enabled` at INFO.
These messages occur once during device startup, never once per stream or transfer. Individual transfer-slot selection
messages remain DEBUG-only.

The backend is named **`cuda-vulkan-direct`**. It registers the actual frame allocation with CUDA:

- RGBA, BGRA, BGRX and ARGB host bytes all use an exportable **RGBA8 UNORM image**. CUDA maps its mipmapped array
  and copies active rows directly between pinned host memory and the base level, respecting the SDK host pitch.
  Channel swizzling, alpha interpretation and color conversion remain shader operations.
- Packed v210 uses an exportable **storage buffer of raw 32-bit words**, including SDK row padding. CUDA copies
  directly into/from that same buffer; the Vulkan packing/unpacking shaders access it without another GPU copy.
- Working surfaces remain four-channel **UNORM16**. Their precision is independent of the byte/word transfer
  representation. Neither a native BGRA image nor a packed RGB/YUV image is introduced at a transfer boundary.

This restores the pre-Vulkan direct-resource design. There is no intermediate CUDA device buffer, no device-to-device
transfer copy, and no format-dependent fallback to Vulkan staging. CUDA selection is strict for every transfer stream once chosen.
A future media format must have a simple, directly shareable byte/word representation with shader conversion;
choosing an incompatible native image format and falling back is not an acceptable integration strategy.

Vulkan owns dedicated exportable allocations and retires them after their final graphics use. CUDA imports memory
and semaphores once per slot; device selection matches the Vulkan UUID. Images are released in `GENERAL` layout and
returned with an explicit external queue-family acquire. Uploads mark the base level changed and generate the requested
mipmaps before publication. The progress worker observes the CUDA completion event before queuing a Vulkan wait, so
an unfinished CUDA transfer cannot block unrelated graphics submissions. A readback lease becomes visible only after
copy completion and the return of graphics ownership.

Host addresses remain aligned, pinned and stable for SDK DMA. CUDA registration is destroyed before the frame allocation,
and external SDK leases still prevent reuse. Both backends implement the same
[direct-memory contract](decklink-direct-memory.md), including the requirements for future DVP support.

The image import descriptor follows NVIDIA's [external-memory mapping requirements](https://docs.nvidia.com/cuda/archive/11.4.3/pdf/CUDA_C_Programming_Guide.pdf):
matching dimensions, channel description and mip count, plus the color-attachment flag for Vulkan render targets.

## Build

From the repository root:

```bash
cmake -S . -B build -DMIXIMUS_ENABLE_CUDA=ON -DBUILD_TESTING=ON
cmake --build build -j 6
```

If discovery needs help, add `-DCUDAToolkit_ROOT=/usr/local/cuda`. CMake should report `CUDA/Vulkan transfers enabled`.
External SDK discovery and linkage live in `src/wrapper/cuda`; only first-party integration code lives in the transfer
implementation. No CUDA language compiler invocation or `.cu` kernel compilation is needed.

`MIXIMUS_ENABLE_CUDA=OFF` builds without the backend. This implementation uses Linux FD export/import; Windows
CUDA/Win32 interoperability is not implemented, and those builds retain Vulkan staging.

## Sanitizer builds

CUDA transfer testing under ASan requires the compatibility defaults documented in [GPU sanitizer
verification](gpu-sanitizers.md). The application, GPU tests and benchmarks now link the same defaults automatically;
ordinary test runs need no manual `ASAN_OPTIONS` or suppression file. Use a Clang 21 ASan/UBSan build and run
`./scripts/test_cuda_transfers.sh build-asan21 1`.

## Test actual CUDA transfers without SDK outputs

```bash
./scripts/test_cuda_transfers.sh build 3
```

The script passes `--use-cuda --log-debug` to the GPU test executable, runs the transfer tests three times, requires log
evidence of completed uploads **and** readbacks, and rejects any Vulkan fallback. It uses synthetic pixels and does not
open NDI/DeckLink sources, transmit video, or create screen outputs. It saves its log under
`build/integration-tests/cuda-transfers-<timestamp>-<pid>/tests.log`.

Expected final line:

```text
PASS: actual CUDA uploads and readbacks completed, pixel/lease tests passed, and no fallback occurred. Log: ...
```

The suite checks padded rows, exact upload selection, retained leases, abandoned targets, concurrent output ordering,
v210 buffers, DeckLink-compatible 4096-byte host alignment, memory budgets, and eight successive full-HD frames with
every active byte and padding checked. It also checks all byte-channel mappings through repeated mipmapped
uploads and minified readbacks, and guards against allocating a second full device frame. A nonzero exit means verification failed; availability/selection alone is
insufficient.

For synchronization validation, use the SDK 1.4.357.0 Khronos layer described in
[Vulkan verification](vulkan-progress.md#build-and-verification) and prefix the test with
`MIXIMUS_VULKAN_VALIDATION=1`. The older 1.4.341 layer can crash in its own concurrent presentation tracking.
A missing requested layer fails the test instead of silently running without validation.

## Test your graph

Start a single mixer instance with debug logging to verify the backend:

```bash
./build/miximus --use-cuda --log-debug --settings build/settings.json
```

Enable the NDI/DeckLink inputs and outputs you intend to test. Text and generated-image uploads also use the same
transfer services. Once selected, CUDA forbids fallback; an incompatible/missing backend produces an explicit `CUDA required;
refusing Vulkan fallback: ...` allocation error. That stream cannot transfer. Merely seeing a running application is not
confirmation that media transferred.

Look for these DEBUG-level events; they are absent at the default log level:

```text
Transfer selected: backend=cuda-vulkan-direct direction=upload ... policy=cuda
Transfer completed: backend=cuda-vulkan-direct direction=upload bytes=...
Transfer selected: backend=cuda-vulkan-direct direction=readback ... policy=cuda
Transfer completed: backend=cuda-vulkan-direct direction=readback bytes=...
```

Selection is logged when each slot is initialized. Completion is logged once per slot after actual CUDA work and its
Vulkan hand-off complete. It is deliberately not logged every frame. Disabled debug logging skips argument evaluation
and formatting; subsequent completions skip the logger entirely. Direction, dimensions, format, and stride identify the
transfer configuration. For a byte-format image, formats `0`, `1`, `2`, and `3` mean RGBA, BGRA, BGRX, and ARGB; `4`
means packed v210.

Screen presentation itself does not read pixels back to the CPU, so it is not a CUDA readback. Its NDI/DeckLink source
transfers can still use CUDA. To verify both directions, use an upload source and an NDI or DeckLink output, or run the
synthetic test script.

## Backend controls and benchmarks

See [recorded CUDA/Vulkan measurements](cuda-transfer-benchmark.md) for results and their implementation scope.
Measurements of the former intermediate-buffer backend do not characterize this direct-resource backend.

Normal runs use staging, including on CUDA-capable machines. `--use-cuda` requests CUDA and runs the startup
qualification checks. Builds without CUDA support and devices without matching CUDA/Vulkan interoperability use staging
automatically. A selected CUDA backend never changes because an individual transfer fails.

Compare separate runs of the same synthetic production-backend workloads:

```bash
cmake --build build --target miximus_transfer_benchmark -j
python3 scripts/benchmark_cuda_transfers.py \
  --output build/integration-tests/cuda-comparison \
  --iterations 500 --repeat 3
```

Use a new output directory for each campaign. Staging runs pass no CUDA flag; CUDA runs pass `--use-cuda`
and the runner rejects them if CUDA was not selected. The runner launches six separate processes in Vulkan/CUDA, CUDA/Vulkan,
Vulkan/CUDA order and pins subsequent runs to the first run's device UUID. Each process tests upload and readback at
720p, 1080p, and UHD in RGBA8 and packed v210, using the same transfer backends as the application. Each
case excludes allocation and 30 warm-up transfers, measures 500 transfers, and checks the final readback bytes. Logs and
per-run JSON include device/driver information, exact commands, executable hash, mean, p50/p95/p99 latency, and
effective GB/s. `summary.md` and `summary.json` contain medians of the per-run statistics. Timing runs disable
application-controlled validation; run without globally forced validation layers, sanitizers, or competing GPU
applications.

These are sequential, host-observed submission-to-completion measurements, including backend ownership hand-offs,
GPU submission-worker scheduling, and readiness polling. They exclude producer pixel generation, color conversion, SDK work, presentation, and
transfer-service queue scheduling. They measure effective backend transfer speed, not bare PCIe bandwidth or full-graph
frame cadence. The benchmark emits its report after measuring; there is no per-transfer logging or formatting. Correctness
tests remain separate from performance measurements.

For a visual application comparison, keep the graph, resolution, buffering, program rate, and output rate identical and
run these commands separately:

```bash
./build/miximus --settings build/settings.json
./build/miximus --use-cuda --settings build/settings.json
```

## Hardware verification

Startup qualification was also tested with injected CUDA import failures: one rejected mipmapped images after
single-level images succeeded; another rejected v210 buffers after all image variants succeeded. In each case all
14 transfer tests passed using staging exclusively, including image transfers whose CUDA probes had already passed.
Both cases ran with synchronization validation and released probe resources before continuing.

On 2026-09-12, the normal build and 132 ordinary tests passed. All 14 GPU transfer tests passed with synchronization
validation with the then-current automatic CUDA selection, forced staging (`--disable-cuda`), and automatic staging
with CUDA devices hidden using `CUDA_VISIBLE_DEVICES`. Logs confirmed actual uploads/readbacks with the expected
backend in each case; the CUDA verification script still rejects staging when checking CUDA operation.

On 2026-09-09, transfer regressions and approved 30-second DeckLink/NDI/screen graph runs completed actual CUDA uploads
and readbacks on a Quadro P2000, with no fallback, orderly shutdown and unchanged saved settings. The later full suite
passed nine transfer tests per backend; see [implementation status](vulkan-progress.md) for the latest validation scope.
These checks establish operation, not visual smoothness or a performance ranking. The [comparison results](cuda-transfer-benchmark.md)
retain the measured performance and its limitations.
