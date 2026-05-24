# Copilot Instructions for miximus

## Project Overview
- **miximus** is a node-based video mixer/compositor, designed for extensibility and modularity. The architecture centers around a graph of nodes, each representing a video processing unit, with connections managed by `core/node_manager_s`.
- The main entry point is `src/main.cpp`, which initializes logging, loads settings, starts the web server, and manages the node graph per frame.
- Nodes and connections are described in JSON config files (see `settings.json`).

## Architecture & Data Flow
- **Nodes**: Defined in `src/nodes/`, registered via `nodes/register_all.hpp`. Each node has interfaces for input/output and options for configuration.
- **Node Manager**: `core/node_manager_s` manages node lifecycle, connections, adapters, and per-frame execution. Adapters (e.g., websocket) allow external control and integration.
- **Adapters**: Extend node manager functionality (see `core/adapters/`). Websocket adapters enable remote configuration and control.
- **Web Server**: `web_server/server.hpp` exposes HTTP/WebSocket endpoints for UI/control.
- **GPU**: Video processing is GPU-accelerated via `src/gpu/`.

## Node System Details
- **Node Definition**: Each node is a video processing unit, defined in `src/nodes/`, inheriting from `nodes::node_i`. Nodes expose interfaces (input/output) and configurable options.
- **Node Registration**: All node types are registered at startup via `nodes/register_all.hpp` and the `node_manager_s` constructor.
- **Node Lifecycle**: Nodes are created, updated, and removed through `node_manager_s` methods (`handle_add_node`, `handle_update_node`, etc.), typically triggered by config changes or external control.

## Adapters & Extensibility
- **Adapters**: Extend node manager behavior. Implement the `adapter_i` interface (see `core/node_manager.hpp`). Adapters can emit node/connection events for external systems.
- **WebSocket Adapter**: Example in `core/adapters/adapter_websocket.cpp`—enables remote configuration and monitoring via web protocols.
- **Adding Adapters**: Use `node_manager_s::add_adapter()` after loading config to avoid event spam.

## Configuration & Data Flow
- **Settings**: Node graph and connections are described in a JSON file (`settings.json`). The main loop loads/saves this file on startup/shutdown.
- **Config API**: Use `node_manager_s::get_config()` and `set_config()` for serialization/deserialization of the node graph.

## Frame Execution
- **Main Loop**: Each frame, `node_manager_s::tick_one_frame()` prepares and executes all nodes, then completes GPU work.
- **Threading**: Frame execution is multi-threaded; thread priority is set for real-time performance (`utils/thread_priority.hpp`).

## Integration & Communication
- **Web Server**: `web_server/server.hpp` exposes HTTP/WebSocket endpoints for UI and remote control.
- **External SDKs**: CEF, DeckLink SDK 16.0, NVIDIA Codec SDK are required for full functionality. Symlink their roots to `3rd-party/` as described in `README.md`.

## Library Versions & Compatibility Notes
- **Boost 1.88**: `io_service` is removed; use `io_context` + `executor_work_guard`. websocketpp uses `get_io_context()`; post via `boost::asio::post(ctx, handler)`.
- **GLFW 3.4 (EGL backend)**: `GLFW_DOUBLEBUFFER` must always be `GLFW_TRUE`; setting it to `GLFW_FALSE` crashes EGL.
- **glad v2** (`glad2` branch): header is `glad/gl.h`, loader is `gladLoadGL(glfwGetProcAddress)`, CMake: `glad_add_library(glad_gl REPRODUCIBLE LANGUAGE C API gl:core=4.6)`. Requires `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` in root `CMakeLists.txt`.
- **DeckLink SDK 16.0** (at `3rd-party/decklink-sdk`):
  - `GetBytes` is no longer on `IDeckLinkVideoFrame`/`IDeckLinkMutableVideoFrame`. Use `QueryInterface(IID_IDeckLinkVideoBuffer, &buf)` then `buf->StartAccess(flags)` / `buf->GetBytes(&ptr)` / `buf->EndAccess(flags)`.
  - `IDeckLinkMemoryAllocator` is removed. Custom allocators now implement `IDeckLinkVideoBufferAllocator` (method: `AllocateVideoBuffer(IDeckLinkVideoBuffer**)`) and return `IDeckLinkVideoBuffer` objects wrapping the actual memory. Providers implement `IDeckLinkVideoBufferAllocatorProvider` and are registered via `EnableVideoInputWithAllocatorProvider`.
  - Linux COM (`LinuxCOM.h`): `REFIID` has no `operator==`; compare with `memcmp(&iid, &IID_Xxx, sizeof(REFIID))`. Use C-style casts `(IInterface*)this` when returning `this` from `QueryInterface` — this is the SDK-canonical pattern. For IUnknown, compare against `CFUUIDGetUUIDBytes(IUnknownUUID)`.

## Shutdown & Lifecycle
- **Destruction order matters**: `web_server` holds a raw pointer to `app_state_s`'s `io_context` (via websocketpp `init_asio`). Declare `web_server` in a tighter scope than `app_state_s` so it is destroyed first.
- **`web_server::stop()`** is synchronous (uses `std::promise`/`future` to wait for the asio thread to finish).
- **DeckLink `uninstall()`** runs with a 2-second timeout via a detached thread to guard against missing kernel drivers.
- **Shutdown watchdog**: `main.cpp` starts a 5-second forced-exit timer after receiving SIGINT/SIGTERM.

## Error Handling & Logging
- **Errors**: Internal APIs return `error_e` enums, not exceptions.
- **Logging**: Use `logger/logger.hpp` and `getlog()` for structured, per-component logging.

## Developer Workflows
- **Build**: Use CMake from the project root:
  ```bash
  mkdir -p build && cd build
  cmake ..
  make -j
  ```
- **Run**: Launch from `build/`:
  ```bash
  ./miximus [--log-debug|--log-trace] [--settings path/to/settings.json]
  ```
- **Settings**: Configuration is loaded/saved as JSON. See `settings.json` for node graph and options.
- **External Dependencies**: Manual setup required for CEF, DeckLink SDK, NVIDIA Codec SDK. Symlink their roots to `3rd-party/` as described in `README.md`.

## Key Files & Directories
- `src/main.cpp`: Entry point, main loop, config load/save
- `src/core/node_manager.hpp/cpp`: Node graph management
- `src/nodes/`: Node definitions and registration
- `src/core/adapters/`: Adapter implementations
- `src/gpu/`: GPU context and processing
- `web_server/`: HTTP/WebSocket server
- `settings.json`: Node graph and config
- `3rd-party/`: External SDKs (manual setup)

---
_If any section is unclear or missing, please provide feedback for further refinement._
