# Miximus agent instructions

Miximus is a C++20 real-time, node-based video mixer and compositor. The native process owns the authoritative graph and 60 Hz render loop; the Vue/Baklava web client is a synchronized remote editor.

## Read the relevant guide first

- [Architecture](docs/architecture.md): runtime ownership, threads, frame lifecycle, graph evaluation, nodes, configuration, WebSocket synchronization, and status deltas.
- [GPU and media](docs/gpu-and-media.md): OpenGL context rules, textures/framebuffers, DVP/CUDA/PBO transfers, device/font registries, callbacks, queues, and workers.
- [Frame timing and synchronization](docs/frame-timing-and-synchronization.md): planned program clock, source alignment, PTS-aware frame selection, buffered outputs, and atomic frame-boundary graph changes.
- [Development](docs/development.md): repository layout, adding native/web nodes, CMake wrappers, dependencies, coding conventions, validation, and shutdown.

More specific `AGENTS.md` files apply inside complex subtrees. Read the nearest applicable file before editing there.

## Repository-wide rules

- The server is authoritative for graph structure and options. Validate native changes before broadcasting them.
- Native and TypeScript node types, interface names, option keys/defaults, protocol enums, conversions, and status keys must remain synchronized.
- Normal `prepare`, `execute`, `complete`, and node destruction happen on the render thread with the root GL context current. Background work is explicit.
- Use `context_scope_s` whenever making a GL context current so stack rewind is RAII-safe, including on early returns.
- Preserve `tick_one_frame()` ordering and the stable `nodes_copy_` render snapshot.
- Use `gpu::transfer::transfer_i` for host/GPU movement. Preserve registration, ownership transitions, completion waits, and teardown ordering.
- Do not expose GPU-to-CPU transfer memory to a worker before `wait_for_copy()` succeeds.
- Do not return pointers or views into registries that may refresh concurrently. Use locking, owned values/handles, and version counters.
- Publish expensive status lists only when a registry version or dependent selection changes. Broadcasts are deltas; initial config and explicit pulls are full snapshots.
- Keep blocking SDK, network, and file work off the render thread. Prefer bounded/free-slot queues and frame drops over render-thread stalls.
- External SDK discovery and linkage belongs in `src/wrapper/`, not directly in consumers.
- Use `std::string_view` only for non-owning inputs with clear lifetimes. Use `utils::transparent_string_hash` for heterogeneous unordered string lookup.
- Use `std::format`, project RAII patterns, `error_e` for expected graph/config failures, and component loggers from `logger/logger.hpp`.

## Validation

- Format touched C/C++ files with `clang-format`.
- Build native changes with `cmake --build build -j` and run `git diff --check`.
- Format touched web files with Prettier and run `npm run build` in `web/`.
- For node/protocol changes, verify both native and web definitions.
- Hardware integrations require runtime testing on suitable DeckLink/NVIDIA/NDI/display hardware; a successful build is not sufficient.

There is currently no substantial automated test suite under `src/`, so targeted builds and runtime checks are important.
