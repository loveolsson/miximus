# CUDA/Vulkan transfers and verification

CUDA transfer support is restored on Linux. Vulkan staging is the default. Pass `--use-cuda` to require CUDA transfers
on the selected Vulkan device. The choice is immutable for the device lifetime; the former
`MIXIMUS_GPU_TRANSFER_BACKEND` environment variable no longer selects a backend.

The backend is named **`cuda-vulkan-buffer`**. It uses CUDA-pinned host storage, a dedicated exportable Vulkan buffer
imported into CUDA, asynchronous CUDA H2D/D2H copies, and external binary semaphores with explicit Vulkan/external
queue-family ownership transfers. CUDA device selection matches the Vulkan device UUID. No OpenGL resource or context is
involved.

CUDA and Vulkan implement the same private [direct-memory transfer contract](decklink-direct-memory.md), preserving the
allocator and SDK lease requirements for a future DVP backend. CUDA remains supported; the P2000 comparison is specific
to that machine and implementation.

For image frames, Vulkan copies between the shared transfer buffer and the raw image. Packed v210 frames use a
buffer-to-buffer copy. Rendering, mipmaps, and color conversion remain Vulkan operations. This restores a real CUDA
transfer path; it does not yet implement direct CUDA access to Vulkan images or establish which path is fastest under a
particular workload.

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
every active byte and padding checked. A nonzero exit means verification failed; availability/selection alone is
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
transfer services. Required mode forbids fallback; an incompatible/missing backend produces an explicit `CUDA required;
refusing Vulkan fallback: ...` allocation error. That stream cannot transfer. Merely seeing a running application is not
confirmation that media transferred.

Look for these DEBUG-level events; they are absent at the default log level:

```text
Transfer selected: backend=cuda-vulkan-buffer direction=upload ... policy=cuda
Transfer completed: backend=cuda-vulkan-buffer direction=upload bytes=...
Transfer selected: backend=cuda-vulkan-buffer direction=readback ... policy=cuda
Transfer completed: backend=cuda-vulkan-buffer direction=readback bytes=...
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

See [recorded CUDA/Vulkan measurements](cuda-transfer-benchmark.md) for the current P2000 results and validation.

Without `--use-cuda`, the application uses Vulkan staging and does not initialize CUDA transfer slots. With the flag, it
requires CUDA and refuses fallback if a transfer allocation cannot initialize. Rebuild with the CUDA toolkit available
to enable the optional backend. The previous environment-variable selector is ignored, including when inherited from a
shell.

Compare separate runs of the same synthetic production-backend workloads:

```bash
cmake --build build --target miximus_transfer_benchmark -j
python3 scripts/benchmark_cuda_transfers.py \
  --output build/integration-tests/cuda-comparison \
  --iterations 500 --repeat 3
```

Use a new output directory for each campaign. The runner launches six separate processes in Vulkan/CUDA, CUDA/Vulkan,
Vulkan/CUDA order and pins subsequent runs to the first run's device UUID. Each process tests upload and readback at
720p, 1080p, and UHD in RGBA8 and packed v210, using the same `frame_staging_s` implementation as the application. Each
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

On 2026-09-09, transfer regressions and approved 30-second DeckLink/NDI/screen graph runs completed actual CUDA uploads
and readbacks on a Quadro P2000, with no fallback, orderly shutdown and unchanged saved settings. The later full suite
passed nine transfer tests per backend; see [implementation status](vulkan-progress.md) for the latest validation scope.
These checks establish operation, not visual smoothness or a performance ranking. The [comparison results](cuda-transfer-benchmark.md)
retain the measured performance and its limitations.
