# CUDA versus Vulkan transfer measurements — 2026-09-09

The application now uses Vulkan staging unless launched with `--use-cuda`. The old environment selector no longer
changes the backend. CUDA-required mode refuses fallback.

On this Quadro P2000, the current CUDA buffer backend was slower in the 720p and 1080p isolated cases. At UHD, packed
v210 favored CUDA slightly; RGBA favored Vulkan. These results compare the current implementations and do not establish
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
verify that CUDA defaults off and `--use-cuda` enables it.

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
