# Miximus agent instructions

Miximus is a C++20 real-time, node-based video mixer and compositor. The native process owns the authoritative graph and 60 Hz render loop; the Vue/Baklava web client is a synchronized remote editor.

## Git authorization

- Do not commit or perform any other mutating Git operation without the user's explicit instruction authorizing that specific operation and scope. This includes staging, switching or creating branches, fetching, pulling, merging, rebasing, cherry-picking, reverting, resetting, stashing, cleaning, and changing Git configuration or refs.
- Instructions to implement, fix, build, test, review, or edit documentation do not authorize Git mutations. Authorization for an earlier operation does not carry over to later changes.
- The agent must NEVER push, including ordinary pushes, force-pushes, and pushes through tools or APIs. Pushing is reserved for the user.
- Read-only Git inspection is allowed. Leave file edits unstaged and uncommitted unless explicitly instructed otherwise.

## Read the relevant guide first

- [Architecture](docs/architecture.md): runtime ownership, threads, frame lifecycle, graph evaluation, nodes, configuration, WebSocket synchronization, and status deltas.
- [GPU and media](docs/gpu-and-media.md): Vulkan recording ownership, textures/framebuffers, bounded transfers, device/font registries, callbacks, queues, and workers.
- [Frame timing and synchronization](docs/frame-timing-and-synchronization.md): planned program clock, source alignment, PTS-aware frame selection, buffered outputs, and atomic frame-boundary graph changes.
- [Windows setup and full-feature port](docs/windows-development.md): clean-clone dependencies, custom CEF source build, CUDA Win32 transfers, implementation gaps, and hardware acceptance. CEF and CUDA are required Windows port deliverables; a core-only build is an intermediate checkpoint.
- [Development](docs/development.md): repository layout, adding native/web nodes, CMake wrappers, dependencies, coding conventions, validation, and shutdown.

More specific `AGENTS.md` files apply inside complex subtrees. Read the nearest applicable file before editing there.

## Repository-wide rules

- The server is authoritative for graph structure and options. Validate native changes before broadcasting them.
- Native and TypeScript node types, interface names, option keys/defaults, protocol enums, conversions, and status keys must remain synchronized.
- Normal `prepare`, `execute`, `complete`, and node destruction happen on the render thread with explicit GPU recording/resource ownership. Background work is explicit.
- Record typed GPU work through `app->commands()`. Publish output leases only after successful submission; `complete()` does not mean GPU completion.
- Preserve `tick_one_frame()` ordering and the stable `nodes_copy_` render snapshot.
- Use the bounded upload/readback services for host/GPU movement. Preserve exact upload IDs, external leases, completion, and teardown ordering.
- Do not expose GPU-to-CPU transfer memory before copy completion and noncoherent-memory invalidation.
- Preserve DeckLink DMA directly into/from backend-owned transfer memory and the external lease lifetime. Keep allocation, registration, and synchronization extensible for future DVP support; see [the direct-memory contract](docs/decklink-direct-memory.md).
- Do not return pointers or views into registries that may refresh concurrently. Use locking, owned values/handles, and version counters.
- Publish expensive status lists only when a registry version or dependent selection changes. Broadcasts are deltas; initial config and explicit pulls are full snapshots.
- Keep blocking SDK, network, and file work off the render thread. Prefer bounded/free-slot queues and frame drops over render-thread stalls.
- External SDK discovery and linkage belongs in `src/wrapper/`, not directly in consumers.
- Use `std::string_view` only for non-owning inputs with clear lifetimes. Use `utils::transparent_string_hash` for heterogeneous unordered string lookup.
- Use `std::format`, project RAII patterns, `error_e` for expected graph/config failures, and component loggers from `logger/logger.hpp`.

## Validation

- Preserve the user's working IDE/build setup. Before changing presets, tasks, launch settings, or build-environment handling, read the [Windows Play regression and acceptance rules](docs/windows-development.md#windows-play-regression-2026-10-06).
- Preserve VS Code's normal CMake kit, variant, target, and debug controls. Workspace settings must supply required dependency/toolchain parameters, feature flags, and the valid local CEF SDK path for every variant, without relying on leftover cache entries. Do not force presets, a build type, or custom configure/build tasks. Preserve the active `build/` cache and the user's selections. Validate configuration with these settings in an isolated build directory, including a variant change.
- A developer-shell build or an up-to-date IDE build does not validate CMake Tools' build environment. Validate an actual compile and link through the selected kit without relying on a developer-shell parent. State explicitly if the IDE button itself was not exercised. Never reconfigure the user's active build just to validate an IDE-settings edit.

- Format touched C/C++ files with `clang-format`.
- Build native changes with `cmake --build build -j` and run `git diff --check`.
- Format touched web files with Prettier and run `npm run build` in `web/`.
- For node/protocol changes, verify both native and web definitions.
- Hardware integrations require runtime testing on suitable DeckLink/NVIDIA/NDI/display hardware; a successful build is not sufficient.

There is currently no substantial automated test suite under `src/`, so targeted builds and runtime checks are important.
