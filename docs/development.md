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

Requirements include CMake 3.28+, a C++20 compiler, and Node.js 22+ for the web client.

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
./build/miximus [--log-debug | --log-trace] [--settings path/to/settings.json]
```

Build the web client directly when working on it:

```bash
cd web
npm install
npm run build
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
6. Use `prepare`, `execute`, and `complete` according to the frame lifecycle in [architecture.md](architecture.md).
7. Add the factory to the group's `register.cpp`.
8. Add sources to the group's `CMakeLists.txt`.
9. Ensure the group is invoked from `nodes::register_all_nodes()`.

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
- `src/web_server/`
- `src/core/adapters/adapter_websocket.cpp`
- `web/src/messages.ts`
- `web/src/App.vue`
- `web/src/server_sync.ts`

Update all applicable layers together. Preserve request token handling, `origin_id` feedback suppression, and the special handling of server-side connection displacement.

## CMake and external libraries

Project targets are composed in the root and `src/**/CMakeLists.txt` files. External libraries are isolated behind wrapper targets under `src/wrapper/`. Follow that pattern rather than adding SDK paths to consumers.

Current wrappers include:

- CUDA via CMake `CUDAToolkit` and `CUDA::cudart`;
- NVIDIA DVP from `3rd-party/dvp170_linux` or `dvp170_win`;
- Blackmagic DeckLink SDK through `3rd-party/decklink-sdk`;
- NDI through `3rd-party/ndi-sdk`;
- NVIDIA Video Codec SDK through `3rd-party/video-sdk`;
- system FFmpeg components;
- stb implementation sources.

CEF remains in the repository but is not enabled by `src/wrapper/CMakeLists.txt`.

The alias paths are symlinks to selected SDK versions. Preserve stable aliases in build files rather than embedding versioned directory names.

Strict warnings apply to project targets after submodules are configured. Third-party sources are isolated and may suppress warnings. Do not globally weaken warnings to accommodate an SDK.

## C++ conventions

- Use C++20 and existing project namespace/layout conventions.
- Follow the established `_s` suffix for concrete structs/classes and `_i` for interfaces.
- Prefer RAII and explicit ownership with smart pointers.
- Use `std::string_view` for non-owning parameters, but do not store it past the owner's lifetime.
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

There is currently no substantial automated test suite under `src/`. At minimum:

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

## Shutdown ordering

Shutdown order is deliberate:

1. Stop the web server and clear adapters.
2. Save authoritative settings.
3. Clear nodes with the root GL context current.
4. Uninstall device discovery.
5. Shut down CUDA/DVP transfer contexts while root GL is current.
6. Destroy root GL and stop/join application service threads.

`web_server` is declared after `app_state_s`, so it is destroyed first; websocketpp retains a raw pointer to the app's Asio executor. A five-second watchdog forces exit if teardown hangs in normal builds; sanitized builds omit it so slow instrumentation and leak reporting can finish. Do not casually reorder these lifetimes.
