# Miximus development guide

## Repository map

- `src/main.cpp`: startup, settings, frame timing, shutdown.
- `src/core/`: application services, graph management, WebSocket adapter, status.
- `src/nodes/`: native node groups, interfaces, connection model, validation, registration.
- `src/gpu/`: OpenGL and transfer infrastructure.
- `src/render/`: CPU surfaces and fonts.
- `src/web_server/`: embedded web transport.
- `src/wrapper/`: CMake targets wrapping system libraries and SDKs.
- `src/utils/`: reusable low-level utilities.
- `web/`: Vue/Baklava remote editor.
- `static/`: build-time web/resource bundling.
- `resources/`: embedded shaders, images, and settings resources.
- `3rd-party/`: SDK installations and stable alias symlinks.
- `submodules/`: source dependencies built with the project.

Read [architecture.md](architecture.md) before changing graph/config/web synchronization. Read [gpu-and-media.md](gpu-and-media.md) before changing GL, transfers, SDK callbacks, registries, or workers.
The current render-thread latency audit and outstanding migrations are tracked in
[render-thread-audit.md](render-thread-audit.md).
The proposed broadcast timing, clock-recovery, frame-selection, output-buffering, and atomic graph-update architecture
is tracked in [frame-timing-and-synchronization.md](frame-timing-and-synchronization.md).

## Building and running

Requirements include CMake 3.28+, a C++20 compiler, Boost with the Fiber, Program_options, and URL development
components, and Node.js 22+ for the web client.

```bash
cmake -S . -B build
cmake --build build -j
```

Enable clang-tidy during native compilation with:

```bash
cmake -S . -B build -DMIXIMUS_ENABLE_CLANG_TIDY=ON
```

The repository-root `.clang-tidy` contains the shared check configuration used by both CMake and supporting IDE
extensions. CMake also exports `build/compile_commands.json` for clangd and other compilation-database consumers.
Precompiled headers are enabled by default for targets that make extensive use of expensive third-party headers and
can be disabled with `-DMIXIMUS_ENABLE_PRECOMPILED_HEADERS=OFF`. They are disabled automatically while clang-tidy is
enabled because the compiler and clang-tidy may use incompatible PCH formats.

### Sanitizers

Configure a separate Clang build when enabling sanitizers so the normal developer build remains unaffected:

```bash
cmake -S . -B build-asan \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DSANITIZE_ADDRESS=ON \
    -DSANITIZE_UNDEFINED=ON \
    -DMIXIMUS_ENABLE_CLANG_TIDY=OFF \
    -DMIXIMUS_TUNE_NATIVE=OFF
cmake --build build-asan -j
```

On Linux, CUDA/OpenGL interoperability may fail during `cudaGLGetDevices()` with `cudaErrorMemoryAllocation` when
AddressSanitizer protects its shadow-memory gap. This happens before Miximus allocates pinned memory, streams, events,
CUDA images, or CUDA pixel buffers and therefore does not indicate exhausted GPU memory. The current CUDA 11.4 setup
has been verified to initialize and exercise both direct-image and pixel-buffer transfers with:

```bash
ASAN_OPTIONS=protect_shadow_gap=0:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
./build-asan/miximus
```

`protect_shadow_gap=0` leaves AddressSanitizer instrumentation enabled, but removes the inaccessible guard over its
unused shadow gap so CUDA can establish its unified virtual-address mappings. Use this workaround only for CUDA-enabled
sanitizer runs. An out-of-memory error in a normal build, or from a later operation such as `cudaHostAlloc()` or CUDA
resource registration, must still be investigated as a real allocation or driver failure.

Sanitized builds omit the shutdown watchdog. Sanitizer instrumentation and leak reporting can make teardown exceed the
normal five-second deadline, and the watchdog's forced `_Exit()` would prevent destructors and sanitizer finalization
from completing.

LeakSanitizer cannot run while the process is being traced. Add `detect_leaks=0` to `ASAN_OPTIONS` only when running
under a debugger or another environment that uses `ptrace`; leave leak detection enabled for ordinary terminal runs.

Run:

```bash
./build/miximus [--log-debug | --log-trace] [--settings path/to/settings.json] [--stop-after seconds]
```

The application logs its process ID during startup. `--stop-after` requests an ordinary graceful shutdown after the
given positive number of seconds and is useful for repeatable runtime and sanitizer checks.

Build the web client directly when working on it:

```bash
cd web
npm install
npm run build
```

Native deterministic tests use GoogleTest and are registered individually with CTest. Run them with:

```bash
ctest --test-dir build --output-on-failure
```

The native build hashes web sources, rebuilds `web/dist` only when needed, and bundles web output and `resources/` into `static_files`. Web-build failures are reported at the end of the native build.

## Adding or changing a node

A complete node generally requires native and web changes.

### Native side

1. Implement a `nodes::node_i` subclass under `src/nodes/<group>/`.
2. Store interfaces as members and construct each with the owning node (`*this`) and its stable protocol name. Interface
   construction registers it automatically; duplicate names fail node construction.
3. Implement `type()` with a stable protocol type string.
4. Implement `get_default_options()` with every persisted option and its canonical default.
5. Implement `normalize_option()` using `normalize_option_value<T>()` where possible. Return `ok` for an unchanged
   valid value, `corrected` after canonicalizing it, and `invalid` for malformed or unsupported input. Common options
   are normalized centrally by `node_i::set_options()`.
6. Use `prepare`, `submit`, `execute`, and `complete` according to the frame lifecycle in
   [architecture.md](architecture.md).
7. Add the factory to the group's `register.cpp`.
8. Add sources to the group's `CMakeLists.txt`.
9. Ensure the group is invoked from `nodes::register_all_nodes()`.

Define runtime status batches in `src/types/node_status.hpp`, describe every field with `BOOST_DESCRIBE_STRUCT`, and
publish them through the typed node-status writer. Do not add raw string-key status writes or `nlohmann::json` members
to a status contract. The native build regenerates `web/src/generated/json_contracts.ts`; unsupported member types stop
generation at compile time.

Node registration also owns persisted schema evolution. Version 1 is implicit for a factory-only registration. `node_definition_s::migrations` is an append-only vector: element 0 migrates version 1 to 2, element 1 migrates version 2 to 3, and so on. The current schema version is derived from the vector length, so every schema bump necessarily has one ordered migration. Keep each node's migration chain in a separate `<node>_migrations.hpp/.cpp` pair; a shared file is appropriate for a templated node family with one shared schema. Option migrations mutate the options object; input/output interface migrations rename the corresponding endpoint of saved connections. Migrations must throw if their claimed source data cannot be converted safely. Do not bump the schema for implementation-only changes.

### Web side

1. Add or update the Baklava definition in `web/src/nodes/`.
2. Match native type, interface names, option keys, and defaults exactly.
3. Register new node types in `web/src/nodes/types.ts`.
4. For a new connection type, update native `interface_type_e`, native conversions, web `interface_types.ts`, and the connection color map.
5. Use focus-tracking option components for editable values that must not be overwritten while typing.
6. Use `StatusDropdownInterface` for server-discovered lists and publish the exact status key natively. Publish entries
   as `settings_option_s` values: `id` is the stable persisted value and `label` is presentation text.
7. Use `NumericInterface` for numeric controls and set its precision, step, and optional bounds according to the domain rather than the JSON/C++ storage type.

The native server remains authoritative; do not solve validation only in the browser.

## Protocol changes

Protocol actions/topics and payloads span:

- `src/types/action.hpp`
- `src/types/topic.hpp`
- `src/types/web_message.hpp`
- `src/web_server/`
- `src/core/adapters/adapter_websocket.cpp`
- `web/src/messages.ts`
- `web/src/App.vue`
- `web/src/server_sync.ts`

Update all applicable layers together. Preserve request token handling, `origin_id` feedback suppression, and the special handling of server-side connection displacement.
The TypeScript `action_e`, `topic_e`, and `error_e` declarations are generated from their native enums; change the C++
definition and let `miximus_typescript_generator` update the client contract rather than editing those declarations in
`messages.ts`.

Define fixed WebSocket envelopes and payload fields as described structs in `src/types/web_message.hpp`. Include
`web_server/typed_server.hpp`, then pass those structs directly to the existing JSON send and broadcast methods;
nlohmann ADL conversion keeps serialization at the transport boundary. Incoming typed commands deserialize to owning
request structs before dispatch. Keep node options and individual node-status values as JSON members where their schemas
are intentionally selected at runtime. The generator writes the complete shared client contracts to
`web/src/generated/json_contracts.ts`; `web/src/messages.ts` only re-exports them.

## CMake and external libraries

Project targets are composed in the root and `src/**/CMakeLists.txt` files. External libraries are isolated behind wrapper targets under `src/wrapper/`. Follow that pattern rather than adding SDK paths to consumers.

Current wrappers include:

- CUDA via CMake `CUDAToolkit` and `CUDA::cudart`;
- NVIDIA DVP from `3rd-party/dvp170_linux` or `dvp170_win`;
- Blackmagic DeckLink SDK through `3rd-party/decklink-sdk`;
- NDI 6 or newer through the system-installed NDI SDK;
- NVIDIA Video Codec SDK through `3rd-party/video-sdk`;
- system FFmpeg components;
- stb implementation sources.

CEF remains in the repository but is not enabled by `src/wrapper/CMakeLists.txt`.

The project-local `ndi` wrapper discovers the NDI headers and library in the platform's standard SDK installation
locations. `NDI_ROOT` or `NDI_SDK_DIR` may select another SDK root, which must contain NDI 6 or newer. Consumers link
the wrapper and do not depend on the SDK discovery mechanism or an external target name.

The alias paths are symlinks to selected SDK versions. Preserve stable aliases in build files rather than embedding versioned directory names.

Strict warnings apply to project targets after submodules are configured. Third-party sources are isolated and may suppress warnings. Do not globally weaken warnings to accommodate an SDK.

## C++ conventions

- Use C++20 and existing project namespace/layout conventions.
- Follow the established `_s` suffix for concrete structs/classes and `_i` for interfaces.
- Prefer RAII and explicit ownership with smart pointers.
- Use `std::string_view` for non-owning parameters, but do not store it past the owner's lifetime.
- For templated node families, pass stable protocol type strings, default display names, and any varying interface names
  explicitly from each concrete factory. Do not infer protocol metadata from the C++ value type through template traits;
  the same value type may back multiple independently versioned node types.
- Numeric options reject non-finite values during normalization. Numeric output interfaces also replace a non-finite
  publication with the type's valid default, so downstream nodes may assume finite graph inputs. Nodes must still guard
  domain errors such as division by zero at the operation that could produce them.
- Ordered string maps use `std::less<>` for heterogeneous lookup.
- Unordered string maps use `utils::transparent_string_hash` plus `std::equal_to<>` when lookup by view is useful.
- C++20 does not have heterogeneous `unordered_map::erase(key)`; find by view and erase the iterator.
- Use `utils::observed_value_s` for node state that represents the last successfully observed setting, registry version,
  or derived input. `observe()` compares and commits immediately; use `would_change()` followed by `commit()` when
  applying the new value can fail. Keep direct compatibility checks when the cached resource already exposes the
  relevant property.
- Use `std::format`, not fmt APIs.
- Use component loggers from `logger/logger.hpp`: `app`, `http`, `gpu`, `nodes`, `decklink`, and `ndi`.
- Use `error_e` for expected graph/config command failures. Follow existing exception usage for startup and unrecoverable construction failures.
- Preserve constness and include dependencies explicitly; do not depend on unrelated transitive includes.

## Concurrency and lifetime review

Before adding a thread, callback, or asynchronously refreshed collection, document:

- which thread owns and mutates each object;
- how values cross threads;
- what mutex/queue/fence provides ordering;
- when workers stop and join;
- whether returned pointers/views remain valid;
- whether GL context ownership is required at construction/destruction.

Mutable registries should protect collections, return owned values or stable handles, increment an atomic version after refresh, and let render nodes publish expensive lists only after version changes.

## Formatting and validation

The focused native suite under `src/core/tests/` covers the reserved settings node, graph lifecycle traversal, the
deterministic scheduler, source-clock recovery, and timed-source queues. It does not replace integration testing. At
minimum:

### Native changes

```bash
clang-format -i <touched C/C++ files>
cmake --build build -j
git diff --check
```

### Web changes

```bash
cd web
npx prettier --write <touched files>
npm run build
```

The web build runs Vue TypeScript checking and a Vite production build.

For changes involving nodes or the protocol, inspect both native and TypeScript definitions. For DeckLink, NDI, CUDA, DVP, font discovery, or display timing, perform runtime validation on suitable hardware; compilation alone cannot validate behavior.

### Long-running timing soak

Use `scripts/test_timing_soak.sh` to run the configured graph for hours while retaining enough evidence to diagnose a
brief output disturbance after it has passed. The runner uses a private copy of the settings file, owns the Miximus
process lifecycle, samples the aggregate `/api/v1/status` endpoint once per second, records CPU/RSS/thread usage, and
records NVIDIA process memory when `nvidia-smi` is available. Scheduler extrema and DeckLink output queue/buffer
counters are cumulative for the lifetime of the active scheduler or output callback, so a transient between HTTP polls
remains visible.

```sh
scripts/test_timing_soak.sh start 8h
scripts/test_timing_soak.sh status
scripts/test_timing_soak.sh stop
scripts/test_timing_soak.sh report
scripts/test_timing_soak.sh brief-report
```

Use `observe DURATION DIR` to sample a developer-run instance without taking ownership of it. `live-snapshot` provides
an immediate native configuration/status view, optionally filtered by exact node type. When a rebuilt binary must
replace one running instance for hardware validation, `restart` first requests a graceful shutdown and then starts a
normal owned soak; it refuses to stop anything if more than one Miximus process exists.

For an A/B soak of the DeckLink output buffer depth without editing the developer settings, pass
`--decklink-output-buffer 4` (or another value from 1 through 8) to `start`. The override is applied only to the run's
private settings copy and is recorded in `summary.json`.

To exercise cadence conversion while retaining the configured application clock, override only the DeckLink display
mode:

```sh
scripts/test_timing_soak.sh start 10m --decklink-output-buffer 5 --decklink-display-mode 1080p59.94
```

The private run settings and summary record the selected mode. When the program and device rates differ,
`program_timing_drops` or `program_cadence_repeats` on the output and `source_queue_timing_repeats` or selection drops
on the looped input are expected cadence decisions, not automatically faults. Compare their rate with the exact
rational cadence difference and separately require the hardware late/drop, starvation, overflow, transfer-failure,
and buffer-underrun counters to remain stable.

To verify that buffered outputs absorb an isolated late render and remain full while the scheduler catches up, inject a
short render-thread stall at a fixed rendered-frame interval:

```sh
scripts/test_timing_soak.sh start 3m --render-delay-ms 12 --render-delay-every 120
```

Choose a delay that makes the affected frame miss its deadline but remains inside the scheduler's allowed-lateness
window once the ordinary render time is included. A successful run records the injection count and deadline misses, no
skipped program frames, and no output late/drop, starvation, refill-shortfall, or buffered-zero events. The injector is
test-only process instrumentation and is inactive unless both arguments are supplied.

Before a multi-day endurance soak, use a campaign to collect comparable outlier distributions from shorter runs:

```sh
scripts/test_timing_soak.sh campaign 8h 30m
scripts/test_timing_soak.sh campaign-status
scripts/test_timing_soak.sh campaign-report
```

The standard campaign alternates DeckLink output buffer depths in a 5/4/4/5 sequence to reduce time-order bias. Every
run has independent raw artifacts under `timing-campaign-*`, while the campaign summary groups late frames, drops,
starvation repeats, input transfer-slot waits and failures, hardware-buffer samples, callback intervals, scheduler
outliers, and memory maxima by buffer depth. Counter totals and sampled scheduler extrema exclude the initial warm-up;
each run summary retains first, warm-up, and final status snapshots so startup behavior remains independently auditable.

Runs are stored under `build/integration-tests/timing-soak-*`; `timing-soak-latest` points to the newest one. Preserve
`events.log`, `status-samples.jsonl`, `system-samples.jsonl`, `config.json`, `settings.json`, and `summary.json` when
reporting a failure. The runner refuses to launch if another Miximus instance is already serving the configured API,
which prevents accidental contention with a developer-run instance.

The DeckLink loopback mode-change test owns the Miximus process, waits for the HTTP API, applies each requested mode,
waits for both output playback and input capture to restart, restores the original mode, and shuts the process down:

```bash
scripts/test_decklink_mode_changes.sh DECKLINK_OUTPUT_NODE_ID 720p60 1080p50 1080p30
```

It runs against a private copy of `build/settings.json` and leaves the developer settings unchanged. Timestamped test
actions and Miximus logs are interleaved on the terminal and retained together under `build/integration-tests/`.
`BUILD_DIR`, `SETTINGS`, `MIXIMUS_API_URL`, `TRANSITION_TIMEOUT_SECONDS`, and `STARTUP_SETTLE_SECONDS` can override its
defaults. `MODE_DWELL_SECONDS` controls how long playback is observed after each completed mode transition.

## Shutdown ordering

Shutdown order is deliberate:

1. Stop the web server and clear adapters.
2. Save authoritative settings.
3. Clear nodes with the root GL context current.
4. Uninstall device discovery.
5. Shut down CUDA/DVP transfer contexts while root GL is current.
6. Destroy root GL and stop/join application service threads.

`web_server` is declared after `app_state_s`, so it is destroyed first; websocketpp retains a raw pointer to the app's Asio executor. A five-second watchdog forces exit if teardown hangs in normal builds; sanitized builds omit it so slow instrumentation and leak reporting can finish. Do not casually reorder these lifetimes.
