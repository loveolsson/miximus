# CUDA transfer measurements

## Direct-resource implementation — 2026-09-11

`cuda-vulkan-direct` imports the actual RGBA8 frame image or packed v210 storage buffer. CUDA copies directly between
that resource and the SDK-facing pinned host address. The former private device buffer and extra Vulkan copy are gone.
The channel-order and raw-word contracts are described in [CUDA transfers](cuda-transfers.md).

The normal Clang 21 RelWithDebInfo build passed, as did 132 ordinary tests, 28 renderer tests, and 14 transfer tests
with each backend. CUDA's suite passed three consecutive runs. GPU tests used synchronization validation; no tidy or
sanitizer build was run. New coverage checks repeated minifying transfers of RGBA/BGRA/BGRX/ARGB with mipmap renewal
and prevents allocating a second full device frame. A 30-second copied NDI/DeckLink/screen graph also completed with
synchronization validation, direct CUDA uploads/readbacks, and clean shutdown.

### Isolated transfers

Six alternating staging/CUDA processes used the Quadro P2000 with driver 580.178.04 and CUDA 11.4.152. Each case
excluded allocation and 30 warm-up transfers, then measured 500 transfers; these are medians of three run means.
Timing excluded validation and sanitizers. Every final pixel check passed; CUDA runs used the direct backend throughout.
These are host-observed submission-to-completion times, including ownership hand-offs and completion polling, not
isolated DMA-engine timings. They do not measure pipelined application throughput or establish a ranking on other GPUs.

| Size | Format | Direction | Vulkan mean ms | CUDA mean ms | CUDA/Vulkan time |
| --- | --- | --- | ---: | ---: | ---: |
| 1280×720 | RGBA8 | upload | 0.883 | 1.055 | 1.20× |
| 1280×720 | RGBA8 | readback | 0.860 | 1.024 | 1.19× |
| 1280×720 | v210 | upload | 0.611 | 0.808 | 1.32× |
| 1280×720 | v210 | readback | 0.601 | 0.802 | 1.33× |
| 1920×1080 | RGBA8 | upload | 1.720 | 1.860 | 1.08× |
| 1920×1080 | RGBA8 | readback | 1.745 | 1.856 | 1.06× |
| 1920×1080 | v210 | upload | 1.226 | 1.377 | 1.12× |
| 1920×1080 | v210 | readback | 1.213 | 1.360 | 1.12× |
| 3840×2160 | RGBA8 | upload | 6.447 | 5.753 | 0.89× |
| 3840×2160 | RGBA8 | readback | 6.608 | 5.567 | 0.84× |
| 3840×2160 | v210 | upload | 4.718 | 4.167 | 0.88× |
| 3840×2160 | v210 | readback | 4.566 | 4.159 | 0.91× |

CUDA is faster in all measured UHD cases. At smaller sizes, the total CUDA path still has higher latency than staging;
removing the extra device copy does not eliminate Vulkan/CUDA ownership synchronization. No fallback is used to obtain
these results. Reproduce with `scripts/benchmark_cuda_transfers.py` as described in [the verification guide](cuda-transfers.md).

### Live graph

Separate 70-second runs used the same copied 1920×1080/60 graph, including NDI loopback, DeckLink Duo (2) capture,
Duo (1) output and the screen window. The first 15 seconds and final two seconds were excluded. The preserved
pre-change Vulkan executable supplied the intermediate-buffer baseline; the developer settings were unchanged.

Two direct runs each used **759 MiB**, compared with **1,094 MiB** for the old CUDA backend: **335 MiB less**. Per-process
SM activity was **34.8% and 39.3%**, versus **41.1% and 44.6%** in two baseline runs. Clocks were not locked
(approximately 1,685–1,721 MHz), and other desktop GPU work varied, so utilization is less stable evidence than memory
and the removed copy. GPU activity is not a percentage of peak arithmetic throughput.

After warm-up, the runs had no transfer failures, render-thread skips, NDI/DeckLink output queue overflows or output
target drops. Screen presentation still recorded missed intervals: 15 and 4 in the direct runs, versus zero and one in the
baseline runs; the first direct run also recorded one screen queue overflow. These short runs do not establish
whether the pre-existing presentation variability changed, and are not a claim that screen stuttering is resolved.

Raw live-run logs/status samples remain under `/tmp/miximus-gpu-comparison/`; the six isolated runs and executable
hashes are under its `direct-transfer-benchmark/` subdirectory. Temporary results are not bundled into the app.

## Historical intermediate-buffer implementation — 2026-09-09

The measurements below used `cuda-vulkan-buffer`, which added a private device allocation and copy. They describe that
superseded implementation, not direct CUDA transfer performance or the pre-Vulkan design. Commands using the current
benchmark executable now measure the direct backend instead.

At the time of these historical measurements, the application used Vulkan staging unless launched with `--use-cuda`. The old environment selector no longer
changes the backend. CUDA-required mode refuses fallback.

On this Quadro P2000, the former CUDA buffer backend was slower in the 720p and 1080p isolated cases. At UHD, packed
v210 favored CUDA slightly; RGBA favored Vulkan. These results compare the implementations at that time and do not establish
a general ranking of CUDA and Vulkan or predict a direct-image CUDA backend.

## Isolated production-backend runs

Six separate processes ran in Vulkan/CUDA, CUDA/Vulkan, Vulkan/CUDA order on the same device UUID. Each
direction/format/size case excluded 30 warm-up transfers and measured 500 transfers per process. The table reports the
median of three per-run means and effective throughputs. Lower time is better; throughput is host payload bytes divided
by observed completion time.

The build used Clang 21, RelWithDebInfo (`-O2`), no sanitizers, and no requested Vulkan validation. The benchmark
invokes the application’s `frame_staging_s` backend directly. Its timer includes CPU submission, external
ownership/semaphore hand-offs where applicable, GPU copies, and host readiness polling. Allocation, host population,
color conversion, transfer-service queues, SDK work, and screen presentation are excluded. No mixer instance ran during
these measurements. Final uploaded/readback bytes matched in every case.

| Size | Format | Direction | Vulkan ms | CUDA ms | Vulkan GB/s | CUDA GB/s |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 1280×720 | RGBA8 | upload | 0.814 | 1.039 | 4.53 | 3.55 |
| 1280×720 | RGBA8 | readback | 0.806 | 0.999 | 4.57 | 3.69 |
| 1280×720 | v210 | upload | 0.571 | 0.754 | 4.35 | 3.30 |
| 1280×720 | v210 | readback | 0.585 | 0.715 | 4.25 | 3.48 |
| 1920×1080 | RGBA8 | upload | 1.671 | 2.013 | 4.96 | 4.12 |
| 1920×1080 | RGBA8 | readback | 1.698 | 1.883 | 4.89 | 4.40 |
| 1920×1080 | v210 | upload | 1.179 | 1.349 | 4.69 | 4.10 |
| 1920×1080 | v210 | readback | 1.153 | 1.302 | 4.80 | 4.25 |
| 3840×2160 | RGBA8 | upload | 6.427 | 6.966 | 5.16 | 4.76 |
| 3840×2160 | RGBA8 | readback | 6.495 | 6.585 | 5.11 | 5.04 |
| 3840×2160 | v210 | upload | 4.548 | 4.354 | 4.86 | 5.08 |
| 3840×2160 | v210 | readback | 4.511 | 4.188 | 4.90 | 5.28 |

Reproduce with a fresh output directory:

```bash
cmake --build build --target miximus_transfer_benchmark -j
python3 scripts/benchmark_cuda_transfers.py \
  --output build/integration-tests/cuda-comparison-new \
  --iterations 500 --repeat 3
```

The runner pins all later processes to the first selected device and rejects backend mismatches, enabled validation,
failed pixel checks, or unsuccessful runs. It makes no per-transfer log calls after the excluded first-use message.

## Validation

The full native build and all 104 ordinary tests passed. The 22 renderer tests and eight transfer tests in each backend
mode passed with synchronization validation. The CUDA benchmark also passed its separate validation run. The CLI tests
verified the then-current opt-in `--use-cuda` behavior. Current builds select CUDA automatically and use `--disable-cuda` to force staging.

## Live application comparison

Four independent 30-second runs used identical copies of the approved graph in Vulkan/CUDA/CUDA/Vulkan order, with
DeckLink Duo (2) input, Duo (1) output, NDI loopback source `miximus-cuda-validation`, and the configured screen window.
Validation was disabled for timing. Each run was sampled at seconds 10 and 25. The table uses the change in cumulative
readback-duration totals divided by the change in completed transfers, then averages the two runs per backend.

**This is output readback pipeline latency:** its clock starts when the rendered target is submitted to the readback service and stops when the worker observes copy completion. It includes pending rendering, worker scheduling/polling, and the transfer. It is not isolated DMA time and should not be compared numerically to the backend-only table above.

| Output | Vulkan mean ms | CUDA mean ms |
| --- | ---: | ---: |
| decklink_output | 3.911 | 6.612 |
| ndi_output | 6.135 | 7.191 |

Both Vulkan runs and the first CUDA run completed 900 readbacks per output in the sampled interval. The second CUDA run
completed 898 and recorded two `gpu_recording_drops` plus two DeckLink cadence repeats. Every run had zero transfer
failures and zero slot-acquisition misses. Screen timing counters also changed during the runs; those changes are not classified as CUDA copy failures. All four processes completed actual uploads and
readbacks using the requested backend and shut down successfully. The original `build/settings.json` hash was unchanged.
Each run deliberately inherited an environment selector opposite to its command-line choice, confirming the retired
selector cannot override `--use-cuda` or the default.

These short runs show higher mean output readback latency with the current CUDA backend. They do not establish visual
smoothness or long-duration cadence behavior. Temporary logs, copied settings and raw snapshots were removed after the comparison concluded.
