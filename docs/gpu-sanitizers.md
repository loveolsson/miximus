# GPU sanitizer defaults and verification

The Vulkan rewrite must preserve the ability to run CUDA under ASan. These defaults are implemented in
[sanitizer_defaults.cpp](../src/sanitizer_defaults.cpp) and linked through
[gpu_sanitizer_defaults.cmake](../cmake/gpu_sanitizer_defaults.cmake) into GPU-linked executables, including the
application, tests and benchmarks. The object file is forwarded to final executables so the sanitizer runtime's weak hooks
do not silently bypass it.

## Defaults and their scope

| Setting | Reason and scope |
| --- | --- |
| `protect_shadow_gap=0` | Required for CUDA initialization on the tested system. Enabled only in ASan builds that include the CUDA backend. CUDA-free builds retain ASan's default guard. This must be selected at build time: ASan initializes before `--use-cuda` is parsed. |
| `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` | Set before `main()` on Linux ASan builds, unless explicitly set in the environment. Keeps Vulkan driver/layer libraries available for exit-time leak inspection and symbolization. Ordinary builds are unaffected. |
| `leak:libnvidia-glcore.so` and `leak:libGLX_nvidia.so` | Linux ASan builds suppress allocations whose stacks contain these NVIDIA driver libraries. NVIDIA's Vulkan ICD uses both, despite their OpenGL-related names. Presentation reproduces leaks through both even with library retention. There is no blanket suppression of XCB, DBus, CUDA, GLFW or application code. |
| System `addr2line` with `allow_addr2line=1` | Retains symbolized reports in Clang ASan builds, preserving the previous workaround for an exit-time `llvm-symbolizer` protocol deadlock after NVIDIA driver use. That historical deadlock was not independently reproduced in this review. |
| `print_stacktrace=1:halt_on_error=1` | Retains immediate, symbolized UBSan failures. |

Disabling the shadow-gap guard is a CUDA address-space compatibility exception; ASan instrumentation and leak detection
remain enabled. Explicit runtime environment options can override ASan defaults for diagnostic comparisons. Normal
testing should use the compiled defaults.

The library-retention option is the [Vulkan Loader's documented sanitizer
support](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md), available in
loaders built with Vulkan headers 1.3.259 or newer. Suppression patterns use [LeakSanitizer's allocation-stack
matching](https://clang.llvm.org/docs/AddressSanitizer.html#suppressing-memory-leaks). Driver-specific suppression can
hide leaks with those driver frames; it should be reconsidered when changing drivers, rather than expanded to cover
unrelated reports.

## Reproduced behavior, 2026-09-09

Test platform: Linux, Clang 21 ASan/UBSan, CUDA 11.4, NVIDIA Quadro P2000, driver 580.178.04. Results apply to this
configuration; they do not establish that every driver/toolkit needs the same exceptions.

- A minimal CUDA program with `protect_shadow_gap=1` failed at `cudaFree(nullptr)` with `out of memory`. With `protect_shadow_gap=0`, initialization, 8 MiB pinned host/device allocations, a host-to-device copy, and cleanup succeeded.
- Without library retention or suppressions, all 22 renderer tests passed functionally, but LSan failed the process with 2,057 bytes in three allocations, including DBus allocations with unloaded callers. Vulkan and CUDA transfer suites likewise passed nine functional tests each but failed leak checking with 19,449 bytes in 27 allocations.
- Library retention alone made both headless renderer and CUDA transfer suites exit successfully without suppressions. Live Vulkan presentation still reported 13,769 bytes in 26 allocations through NVIDIA driver frames, so retaining libraries did not justify removing the driver suppressions.
- The isolated window suite passed all four functional tests. Suppressing only `libnvidia-glcore.so` left NVIDIA ICD/XCB reports; adding `libGLX_nvidia.so` suppressed those too. Separate leaks on the XCB → Xlib → GLFW event-polling path remain: 64 bytes in two allocations in the suppression comparison and 96 bytes in three allocations in the final run with synchronization validation. These remain unsuppressed and need separate investigation; the window suite is not leak-clean on this system.
- With the final defaults, deliberate application use-after-free, signed integer overflow, and a 123-byte leak each produced the expected diagnostic and exit status 1. Heap redzones remained poisoned. These checks passed both with and without the CUDA compatibility define.
- Final normal and ASan/UBSan builds passed, as did all 104 ordinary sanitizer tests, 22 renderer tests, and nine transfer tests per backend. The hardware suites ran with synchronization validation. The window suite's unsuppressed leak failure is recorded separately rather than counted as a passing process.
- Two 30-second runs of the copied DeckLink/NDI/screen graph, one using Vulkan staging and one requiring CUDA, completed upload and readback, passed synchronization validation, and shut down with exit status 0. Each used the automatic defaults, with no manual sanitizer options. Driver suppressions were exercised; there were no unsuppressed sanitizer errors. Saved user settings were unchanged.

## Running the checks

Use the SDK 1.4.357.0 validation layer and environment described in
[Vulkan verification](vulkan-progress.md#build-and-verification). The old 1.4.341 layer's concurrent presentation
history tracking has a reproduced heap-use-after-free. Updating that optional tool preserves synchronization
validation with default locking; it does not require changing the CUDA shadow-gap exception, leak detection,
driver-library retention or application synchronization.

Configure a separate build using the [Clang 21 ASan/UBSan instructions](development.md#sanitizers), with
`-DMIXIMUS_ENABLE_CUDA=ON`. For a build named `build-asan21`:

```sh
cmake --build build-asan21 -j
ctest --test-dir build-asan21 --output-on-failure
build-asan21/src/gpu/gpu_vulkan_test
build-asan21/src/gpu/gpu_transfer_vulkan_test
./scripts/test_cuda_transfers.sh build-asan21 1
build-asan21/src/gpu/gpu_window_test
```

Run on suitable hardware in a normal terminal with leak detection enabled and no manual sanitizer overrides. LSan
requires process inspection and fails in the agent's restricted execution sandbox; these checks were run outside that
sandbox. Do not disable leak detection to obtain passing results. Vulkan synchronization validation remains a separate
check from host ASan/UBSan.

The CUDA script passes `--use-cuda`, verifies completed upload and readback through `cuda-vulkan-direct`, and rejects
fallback. Driver suppression summaries at exit are expected; unsuppressed sanitizer reports still fail the process.
