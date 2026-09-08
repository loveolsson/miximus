# Vulkan migration proposal

Status: proposed for review; no implementation authorized by this document. Research date: 2026-09-08.
Repository baseline: `cda42ce`.

Read this document for the architecture and implementation sequence, then
[transfer strategy](vulkan-transfers.md) for platform choices and
[validation and performance gates](vulkan-validation.md) for acceptance criteria.
Existing behavior is documented in [GPU and media](gpu-and-media.md) and
[frame timing and synchronization](frame-timing-and-synchronization.md).

## Recommendation and difficulty

Replace the OpenGL implementation with a Vulkan-oriented GPU subsystem. Keep ordinary nodes concerned with images,
render targets, drawing operations, and media tickets. Do not preserve current contexts, bind/unbind methods, shader
objects, or fence classes simply for compatibility. Platform presentation and media interop may use native APIs inside
dedicated implementation files; this does not require a second rendering backend.

The compositor itself is a manageable rewrite: a small set of quad shaders, render targets, blends, and conversions.
The difficult part is proving asynchronous resource ownership, display pacing, and transfer performance under concurrent
DeckLink/NDI/render workloads on different memory architectures. Vulkan will give us more control; it does not by itself
guarantee faster transfers or better tail latency.

## Execution order: Linux first

Perform the rewrite and local benchmarks on this Linux machine, targeting native Wayland. Complete a useful Linux
implementation and tune its real workloads first. Attempt reasonable macOS and Windows implementations from the
shared design and documentation, using available build checks, without requiring remote hardware validation before
Linux can progress. Then move to Mac for benchmarking and optimization, followed by Windows.

Linux performance is part of the initial design work, not only post-cutover tuning. Benchmark representative transfer
and concurrent render workloads in the early probes and vertical slice, before committing to storage, conversion,
queue topology, and submission interfaces. Revisit those choices when measurements expose a larger cost; repeat the
benchmarks as integration proceeds.

This machine's Quadro is one hardware sample. Its results describe this GPU, driver, memory/PCIe topology, and workload;
they must not be generalized to all Linux systems or even all NVIDIA GPUs. Use local evidence to inform the architecture
while retaining alternative storage/transfer paths and device-specific selection. Distinguish portable correctness
requirements from measured local preferences and untested hypotheses about other hardware.

DVP is removed entirely as part of the rewrite: backend, selection logic, initialization/shutdown, wrapper linkage,
and obsolete documentation. Do not investigate or implement a DVP/CUDA bridge. Compare native Vulkan transfers with
optional CUDA external-memory paths where local measurements justify them.

The first pass need not settle every optimization or portability detail. Preserve correctness and bounded ownership,
measure Linux performance, and record known gaps for later platform passes. Mac/Windows hardware availability,
perfect performance parity, and long endurance runs are not prerequisites for the initial Linux cutover. The previous
combined three-platform effort estimate no longer describes this sequence; re-estimate the remaining work after the
local vertical slice and separately at each platform handoff.

## Scope and preserved boundaries

The final application renders and performs ordinary GPU copies/conversions through Vulkan on Windows/Linux and
MoltenVK on macOS. CUDA, Metal/CoreVideo, and other SDK APIs remain legitimate optional interoperability mechanisms.
No OpenGL context or GL interoperability bridge remains in the final application.

Preserve these behaviors while changing implementation:

- Authoritative native graph, serialized configuration, stable `nodes_copy_`, interface identity, and web protocol.
- All-node preparation/completion, conservative submission traversal, lazy once-per-frame execution, and hot sources.
- Exact PTS-selected source tickets: a late selected transfer must not silently become an older frame.
- Bounded input/output pools, explicit repeat/drop decisions, and SDK lease ownership through actual completion.
- CPU rendering of text and surfaces; moving these generators to GPU rendering is not a prerequisite.

Keep normal node lifecycle on the render thread. Replace the requirement for a current GL context with recording and
resource-lifetime rules. Update the existing architecture documents and applicable `AGENTS.md` rules when the cutover
lands; their current GL/DVP priority instructions describe the old backend, not this proposed end state.

Ordinary node headers must not include Vulkan, CUDA, Metal, or platform memory handles. A screen presenter or decoder
adapter may legitimately have specialized native code, preferably under `detail/` or `src/gpu/interop/`. Avoid creating
a generic abstraction for an API used by only one specialized integration.

## Current implementation and replacement map

| Current code | Proposed replacement |
| --- | --- |
| `src/gpu/context.*`, `core/app_state.*`, `core/node_manager.cpp` | Device owner, recording context, submission/completion service; independent window/presentation service. |
| `texture.*`, `framebuffer.*`, `texture_frame.*`, `fence.*` | Backend-owned images and retained frame leases; explicit resource use and completion tokens. |
| `shader.*`, `draw_state.*`, `vertex_array.*`, `vertex_buffer.*`, `textured_quad.*` | Precompiled shaders, cached pipelines, typed draw parameters, reusable quad renderer. |
| `resources/shaders/` | Vulkan GLSL compiled to SPIR-V; preserve color/packing math initially. |
| `gpu/transfer/texture_upload.*`, `texture_readback.*`, `detail/` | Bounded services retained conceptually; storage may be a buffer, image, or imported surface. |
| `nodes/composite/`, `utils/`, `debug/`, `generators/`, `text/`, `teleprompter/` | Use the new recording/drawing and upload APIs, without native graphics details. |
| `nodes/decklink/detail/`, `nodes/ndi/detail/` | Keep SDK control/timing logic; replace allocation and conversion integration. |
| `nodes/screen/detail/output_presenter.*`, `gpu/detail/monitor_platform_*` | Vulkan swapchain presenter; retain monitor identity, color information, and timing policy. |
| `wrapper/`, `gpu/CMakeLists.txt`, resource bundling | Vulkan/toolchain dependencies and optional vendor interop. |

The four raw GL calls outside `src/gpu` are in `nodes/screen/detail/output_presenter.cpp` (`glViewport`, `glClearColor`,
`glClear`, `glEnable`). They are a small part of the work; implicit GL semantics inside the wrappers are the real change.
`nodes/ffmpeg/player.cpp` currently has empty preparation/execution methods: codec integration is new functionality,
not an existing feature that must be ported for parity.

## Proposed GPU design

### Device and recording

Use one selected physical/logical device initially. Enumerate actual rendering, presentation, format, memory, and
interop capabilities; choose by device identity and workload rather than renderer-name matching. Support explicit
device selection for reproducible deployments. Record why optional paths are unavailable. Cross-adapter transfer is a
separate capability, never an assumption based on matching vendor names.

Proposed feature floor: Vulkan 1.3 with dynamic rendering, synchronization2, and timeline semaphores enabled after
querying support. Keep descriptor binding conventional; descriptor indexing, buffer device addresses, Vulkan Video,
host-image-copy, and vendor extensions are optional. Confirm the floor on this Linux machine in stage 0, then on each later platform, instead
of developing a second legacy synchronization path preemptively.

For macOS, initially pin a MoltenVK release and implement against its documented capabilities; in the Mac pass test it, enable portability enumeration and advertised portability-subset support,
and inspect subset features. The current upstream guide lists the proposed core facilities and Metal interop extensions;
it currently requires macOS 12 or newer. That is research evidence, not a guarantee about every packaged release or GPU.
[MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)

Keep the main thread as the graph recorder. A small `render_context_s` records typed operations into command buffers
and tracks resource dependencies; it is passed explicitly during execution. `gpu::device_s` owns resources and caches.
These are proposed roles, not a prescribed class hierarchy. Avoid building a second graph compiler or a general-purpose
rendering engine. Begin with ordered operations matching the existing lazy graph traversal.

Record resource usage where drawing/copying happens. Track image subresource layouts, access scopes, and queue-family
ownership centrally. Preserve ordered mutable-framebuffer semantics, including dependencies across program frames.
An upstream target must be resolved before opening the downstream rendering scope; nested graph resolution must not
accidentally nest rendering scopes. Merge adjacent compatible draws only after correctness is established.

Use bounded command/descriptor/parameter arenas, retained until GPU completion. Batch meaningful work rather than
submitting once per node. Start with a graphics/compute queue for correctness; add transfer queues only when they
improve the measured graph. Each recording thread owns its command pool. Centralize synchronization of access to each
actual queue, including queues shared with external libraries. More Vulkan queues do not establish more hardware engines.
[Vulkan command-buffer rules](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)

### Completion and ownership

A completion token identifies submitted work, typically a queue timeline and value. It supports dependency recording,
nonblocking readiness checks, and cancellation-aware worker waits. A generic token may wrap external synchronization
without pretending that every primitive is a `VkFence`. Use binary semaphores where WSI or an external API requires
them. [Vulkan synchronization](https://docs.vulkan.org/guide/latest/synchronization.html)

An upload selected during `submit()` remains that exact selection. Initially the existing exact-upload wait can stay
in `execute()` while the new backend is validated. Subsequently, when a resource and valid submitted dependency exist,
record a GPU wait instead of making the CPU wait for the entire transfer. Do not enqueue an unresolved wait that can
prevent the work which signals it from being submitted. Missing/failed/cancelled producers need explicit terminal states.
This changes the mechanism of the timing contract, not its selection policy.

At frame submission, associate actual resource uses with completion tokens. `complete()` remains a CPU lifecycle hook;
it must not imply GPU completion or create one fence per consumed node. Retire resources only after all GPU uses and
external lease holders finish. Cancelled recordings must release their references without waiting on tokens that will
never be signalled. Deferred destruction also covers pipelines, descriptors, imports, and resized targets.

### Rendering, shaders, and formats

Expose operations such as clear, draw texture, mix textures, and convert media through typed parameters. Put conversion
implementation in the GPU layer; nodes choose colorimetry, geometry, and output requirements. Use immutable pipeline
state and per-draw parameter storage instead of shared mutable named uniforms. Small parameters can use push constants;
larger blocks use aligned uniform buffers. Verify C++/shader layouts, especially `mat3` padding and combined block size.
[Shader data mapping](https://docs.vulkan.org/guide/latest/mapping_data_to_shaders.html)

Compile shader resources to SPIR-V at build time with a pinned GLSL toolchain, explicit locations/bindings, and a single
include mechanism for shared color functions. Validate the modules and warm the finite set of needed pipelines off the
render thread. Key persistent pipeline caches by device/driver identity and shader/interface version; discard invalid
caches. [Khronos glslang](https://github.com/KhronosGroup/glslang)

Use a generated quad or small immutable vertex buffer; a VAO class has no reason to survive. Default to color-only
render targets: current framebuffers allocate depth/stencil storage even though these compositor operations do not
require it. Define premultiplied blending, viewport/scissor, and clear/preserve behavior explicitly.

The source currently uses `GL_RGB16` and `GL_RGBA16` **UNORM**, not floating-point storage, despite some prose describing
RGBA16F. Record numerical baselines before choosing the new working format. Prefer four-channel working targets for
portability; evaluate `R16G16B16A16_UNORM` and `R16G16B16A16_SFLOAT` for required attachment, blending, and sampling
features. Treat a precision/range change as a reviewed color decision, not an incidental porting fix. Three-component
16-bit storage is not a hard requirement.

Retain raw byte-compatible transfer formats and shader packing/swizzling. Image-view swizzling is an optional shortcut,
not a replacement for the portable shader path; portability-subset support explicitly includes a swizzle feature.
[Portability subset features](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePortabilitySubsetFeaturesKHR.html)

Generate mipmaps only for consumers that need minification, after the final base-level write and before sampling.
Use supported image blits or a shader downsample path. Integer packing buffers/images do not get filtered mip chains.
Resolve coordinate origin once in the renderer/presenter, with asymmetric test images; remove scattered compensating
flips. Retain linear Rec.709 working primaries, conversion matrices/ranges, premultiplied alpha, and display encoding
unless a separately tested correction is agreed.

### Presentation

Retain GLFW for window creation/events and create windows with `GLFW_NO_API`; obtain required instance extensions and
create Vulkan surfaces through its supported interface. Window/event operations stay on the required main thread.
[GLFW Vulkan guide](https://www.glfw.org/docs/latest/vulkan_guide.html)

Keep the screen output's bounded offscreen program slots and separate presenter. The presenter acquires a swapchain
image, selects against its output timeline, performs the final draw, and presents. A minimized/resizing/occluded window
must not make the program renderer wait for image acquisition. Check surface support per monitor/device; handle
out-of-date, suboptimal, lost-surface, DPI, and refresh changes through finite retirement/recreation.

Keep GPU render completion, present-wait semaphore consumption, and physical display timing distinct. Submission fences
alone do not establish that presentation has finished consuming its semaphore.
[Swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)

Use FIFO as the initial cadence-preserving mode; probe supported timing/present-wait extensions and report the quality
of timing observations. Never treat a GPU timestamp or return from present as exact scanout. Preserve the existing
clock estimator and repeat/drop policy with an explicit estimated-cadence fallback. Choose swapchain format/color space
and display encoding together so sRGB encoding occurs exactly once.

Prefer a separate presentation-capable queue when useful, but do not promise isolation merely from a separate thread
or queue. Test shared-queue host blocking and MoltenVK's actual scheduling early. Never hold a common queue lock while
waiting for a drawable or CPU completion, and never put an unready display acquisition ahead of unrelated program work.
Native presentation specialization is acceptable if measurements show it is needed to meet the isolation requirement.

## Build and migration sequence

Keep external dependency discovery under `src/wrapper/`. Add Vulkan loader/headers, a shader compiler, and Vulkan Memory
Allocator for ordinary allocation/pooling. Keep external-memory allocations in explicit interop code where requirements
differ. VMA already distinguishes sequential host writes, random host access, and readback usage.
[VMA usage guidance](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html)

Remove DVP entirely. Make CUDA and NVIDIA codec discovery/linkage optional and private to their implementations. The current mandatory
`find_package(CUDAToolkit REQUIRED)` and NVIDIA codec libraries prevent the intended vendor-independent build. Build
Apple bridges as Objective-C++ in narrow wrapper/interop targets. Remove GLAD, Linux GL/GLX linkage, and forced-X11
initialization at Linux cutover. Native Wayland is the target window system. Replace or isolate the current Xrandr
monitor discovery/color metadata implementation; document unavailable compositor capabilities explicitly. Removing GLX
alone does not port monitor discovery/color handling. X11/XWayland may provide an old-backend comparison, but are not
acceptance targets for the new renderer.

| Stage | Deliverable and gate |
| --- | --- |
| 0. Local evidence | Inventory this Quadro system and capture current images/timing/throughput using working non-DVP paths. Probe and benchmark Vulkan transfer/storage alternatives, DeckLink allocation, and Wayland presentation locally. Record comparison limitations and hardware-specific conclusions. |
| 1. GPU foundation | Device, memory, recording, completion, retirement, diagnostics, and offscreen tests build/run on Linux. Keep platform dependencies isolated and remove DVP integration. |
| 2. Local vertical slice | CPU pattern → upload → draw/conversion → readback, plus native Wayland screen output. Run correctness tests and concurrent performance comparisons of storage, conversion, and queue/submission choices. Revise the design from these results before broad node integration. |
| 3. Linux app cutover | Migrate app ownership and ordinary nodes; integrate DeckLink/NDI and preserve timing/protocol behavior. Remove the old GL backend and dependencies once the Linux path is usable. Record measured regressions for iteration rather than requiring a perfect first port. |
| 4. Linux tuning and portability attempt | Extend the early benchmarks to the integrated workloads and tune native Vulkan, buffer conversion, host import, optional CUDA, and NDI format choices on this machine. Implement reasonable Mac/MoltenVK and Windows paths without those machines; perform available compile/static checks and mark hardware behavior untested. |
| 5. Mac pass | Move to Mac; fix build/runtime and MoltenVK issues, benchmark shared-memory and transfer candidates, optimize presentation and media workloads, and record platform results. |
| 6. Windows pass | Move to Windows; fix build/runtime and interop issues, benchmark and optimize transfers/presentation, and record platform results. |
| 7. Hardening and future media | Extend hardware coverage and endurance tests as available. Implement future FFmpeg timed playback/native surfaces as a separate feature package using the established ownership model. |

Develop stages 0–2 in a separate probe/test target alongside the working application. For stage 3, use a short-lived
compile-time migration target if incremental integration requires it; do not create a permanent dual-backend framework
or mixed GL/Vulkan frame pipeline. Keep the old revision runnable for comparisons until acceptance.

## Direction and remaining design decisions

1. Vulkan-first replacement with freedom to change GPU classes and node-facing operation APIs; ordinary nodes remain
   graphics-API independent.
2. Linux/Wayland rewrite and benchmarks on this machine first, a reasonable blind Mac/Windows implementation, then
   hardware optimization on Mac followed by Windows. Remove DVP entirely; no DVP replacement bridge.
3. Workload-specific measured transfer selection; no permanent vendor-priority chain and no presumption that fewer
   named copies always means less latency.
4. Current functionality and transfer tuning as the migration scope; future codec functionality follows separately,
   with GPU-native frame ownership designed in from the start.
5. Vulkan 1.3 as the proposed baseline, with working-format choice informed by local probes and other platform
   compatibility resolved during their respective passes.

The Linux-first order and DVP removal reflect the user’s revised scope. Remaining architecture details are proposals.
Experimental fast paths need correctness checks before use and measurements before being called faster; untested
Mac/Windows paths are expected intermediate results, not blockers for Linux development.
