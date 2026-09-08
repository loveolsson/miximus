# Vulkan transfer strategy

Status: proposed; research 2026-09-08, repository `cda42ce`. Part of the
[Vulkan migration proposal](vulkan-migration.md). No performance claims below have been benchmarked for this project.
Use the [validation plan](vulkan-validation.md) to decide which candidates ship and which become defaults.
Implementation and performance work start on this Linux/Wayland machine. Mac/Windows initially receive reasonable
untested implementations; hardware tuning follows on Mac, then Windows. DVP is removed, not ported.

Compare transfer/storage candidates during the early Linux probes and vertical slice so their performance informs
the service interfaces and resource model before broad integration. This Quadro's ranking is a local measurement,
not a Linux-wide or NVIDIA-wide ranking. Preserve the ability to select different paths on other devices, including
devices running the same OS or belonging to the same vendor.

## What “fastest available” means

Choose the fastest **correct complete path for the endpoint and workload**. Optimize producer-ready to usable-frame
latency and sustained concurrent throughput, subject to bounded memory, rendering deadlines, and required color quality.
Also measure CPU occupancy and p95/p99 latency. Copy bandwidth in isolation is insufficient.

Native storage remains an important goal. Preserve producer bytes until a GPU operation actually needs another
representation, and pack consumer bytes on the GPU. However, direct sampling of host-visible memory on a discrete GPU
may cost more than one copy followed by repeated device-local sampling. On unified memory, a buffer-to-image copy may
still improve tiling and repeated sampling. Conversely, a one-pass unpack or pack can often work directly with a buffer.
These are competing designs to measure, not a universal zero-copy rule.

Distinguish CPU memcpy, GPU local copy, host/device traffic, layout conversion, API ownership handoff, and SDK-internal
work. An imported handle avoids an application copy only when both APIs can use the same underlying allocation. Host
pointer import does not turn ordinary host memory into a device-to-device SDI DMA path.

## Actual workloads

| Endpoint | Current representation and ownership | Vulkan baseline and candidates |
| --- | --- | --- |
| DeckLink capture | Custom allocator gives the SDK upload-lease host memory; v210, SDK row stride/alignment, fresh lease per write access. Exact timed selection starts the upload. | SDK-compatible host allocation → transfer buffer → raw `R32_UINT` image → unpack. Compare buffer-reading unpack, host import, and NVIDIA interop. Preserve DMA ownership and allocator reuse. |
| DeckLink scheduled output | Render/scale, GPU v210 packing, host readback; keyed output uses premultiplied ARGB. Scheduled SDK frames retain host leases through completion. | Image → packed image → mapped readback buffer initially. Compare compute packing into a device-local buffer plus copy, or directly into host-visible output storage. Keep ARGB mode compatibility. |
| NDI receive | SDK-owned BGRA/BGRX frames are copied into upload leases; source timing selects the exact upload. | One row-aware copy into persistent host-visible storage, then upload/unpack. Compare native YUV receive, eligible host import, or an available custom allocator. |
| NDI send | GPU Rec.709 encoding into RGBA readback; asynchronous SDK send retains the host lease. | Same byte contract initially. Compare GPU packing to UYVY/UYVA and direct output-buffer conversion. |
| Text/teleprompter | CPU surfaces draw into upload leases and request read/write memory. | CPU-cached mapped memory; upload on change. Benchmark shared image/buffer options on UMA without making CPU drawing read write-combined memory. |
| CPU test patterns/generators | Full-overwrite RGBA leases. | Sequential-write mapped buffers; consider different memory types from CPU read/modify/write surfaces. |
| Screen output | GPU-only offscreen slots, one final display draw, no host readback. | Remain GPU-only. Optimize presentation independently of host transfer selection. |
| Future FFmpeg | Player is currently a stub. | Import native decoder surfaces when supported; explicit GPU copy if required; bounded host transfer as the functional fallback. |

Evidence: `src/gpu/transfer/texture_transfer.hpp`, `detail/transfer_layout.cpp`,
`src/nodes/decklink/detail/{allocator,input_capture,output_path,output_video_buffer}.*`,
`src/nodes/ndi/detail/{input_capture,output_sender}.*`, `src/nodes/ndi/output.cpp`, and CPU producer nodes.

## One planner, several storage paths

Retain bounded upload/readback services and move their shared ownership/accounting machinery forward. Replace the rule
that every slot owns one texture and one texture-copy backend. A transfer plan may instead consume/produce a buffer,
an image, or multiple externally owned planes. Avoid a second independent allocation/synchronization framework for
each SDK.

The request should describe:

- Dimensions, pixel packing, plane offsets/strides, actual allocation extent, alignment, color metadata, and alpha mode.
- Direction and origin: CPU writer, SDK host DMA, retained SDK host frame, or GPU-native surface.
- Allocation control: can Miximus provide the memory, or must it import/retain a producer allocation?
- CPU access pattern, device identity, synchronization contract, pool retention, and memory/latency budget.
- Intended use: one-pass conversion, repeated sampling/mipmaps, CPU consumption, SDK DMA read, or codec/presentation use.

The selected plan owns storage and conversion together. It reports the layout and encoding it actually requires, plus
the dependency and lifetime rules. A node should request “export this image as v210 with these output parameters,” not
select a CUDA image or arrange a Vulkan buffer barrier.

Start with the current packed formats. Add plane metadata when implementing NDI YUV and decoder surfaces, rather than
inventing an exhaustive universal media schema. Channel order and bit packing belong to storage metadata; color range,
primaries, transfer function, chroma siting, and premultiplication remain separate.

### Candidate A: Vulkan mapped staging buffers

Required functional baseline on all platforms. Create reusable host-visible allocations once and expose the appropriate
mapped slice directly to writable/readable leases. CPU producers write into the final staging allocation; do not add
another staging memcpy. Use buffer/image copies with explicit layouts and dependencies.

Prefer sequential-write memory for overwrite-only producers and CPU-cached memory for readbacks/read-modify-write.
Honor noncoherent flush/invalidate ranges and atom-size alignment. Ensure two independently owned slots cannot race
through the same noncoherent atom. GPU completion precedes host readback publication. Host coherence never means
that simultaneous CPU/GPU access is safe. VMA provides suitable allocation flags and cache-maintenance helpers.
[VMA usage patterns](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html)

For SDK DMA, prove that the allocation is acceptable to that SDK/driver. A pointer returned by mapping Vulkan memory
is not automatically suitable for device DMA or page registration. If not, use a registered host allocation imported
into Vulkan, or one explicit copy from SDK-compatible host storage. Report that copy honestly.

Validate row-pitch/offset requirements, `bufferRowLength` in texels rather than bytes, image usage/format features,
queue-family transfer granularity, and external allocation requirements. Preserve padding and permitted access extents;
never infer that an SDK pointer has extra bytes before or after its advertised range.

### Candidate B: direct GPU conversion from/to buffers

For v210 input, read raw 32-bit words in a shader from a buffer, unpack/color-convert once into the working image,
and avoid materializing an integer input image when that wins. On a discrete GPU compare staging → device-local buffer
→ conversion against staging → image → conversion. On UMA also test conversion reading the shared host-visible buffer.

For output, let a compute shader pack v210 or RGBA-family bytes into an exact-pitch buffer. On discrete GPUs compare a
device-local packed buffer plus DMA copy against direct writes into host-visible output memory. On UMA test direct
shared-buffer output first, while retaining the image/copy competitor. The output SDK still receives a completed,
retained host pointer. Do not require render-to-linear-image support to enable this path.

This is a proposed optimization with substantial architectural value: the readback service no longer needs to force
every export through a framebuffer. First preserve existing scaling/chroma/rounding behavior; then benchmark fusing
scaling and packing. Fusion is acceptable only with equivalent filtering and color behavior. Initialize required row
padding and handle partial v210 groups deterministically.

### Candidate C: imported host allocations

`VK_EXT_external_memory_host` allows importing host allocations, with implementation-specific memory types and pointer/
size alignment. The allocation must outlive its Vulkan import. Successful import does not remove synchronization or
guarantee suitability of arbitrary SDK pointers.
[Host memory extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_host.html),
[import validity requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html)

Candidate uses are controlled DeckLink allocator pools, aligned CPU surfaces, and retained SDK receive buffers whose
contracts actually satisfy the requirements. Query the actual pointer and buffer usage, not just extension presence.
Allocate SDK pools with the combined requirements from the start. Do not round a foreign pointer down and import pages
outside its owned allocation. Cache imports by retained allocation identity, not recycled pointer address alone.

A persistent imported pool may eliminate a copy; per-frame registration or long retention of SDK buffers may make it
slower or exhaust the producer pool. Compare against copying promptly and returning the SDK frame.

### Candidate D: host image copy

Probe `VK_EXT_host_image_copy` or its core availability, including feature, format, supported layouts, and reported
performance properties. It permits host/image transfers without the usual staging sequence; it is not automatically
asynchronous or free of CPU work. Run host-copy work on workers, retain destination/source ownership, and benchmark
against staging for both sparse updates and full-rate video.
[Khronos host-image-copy sample](https://docs.vulkan.org/samples/latest/samples/extensions/host_image_copy/README.html)

Do not build the baseline around it. A device advertising an extension is a reason to test a candidate, not a ranking.

## Platform-specific paths

| Platform/device | Always establish | Additional candidates |
| --- | --- | --- |
| Windows/Linux NVIDIA | Native Vulkan buffers/copies and buffer conversion | CUDA external memory/semaphores; platform-native codec surfaces. |
| Windows/Linux AMD/Intel | Native Vulkan buffers/copies; UMA-aware memory choices where applicable | Host import/copy extensions; Windows shared D3D resources or Linux DMA-BUF for media; native Vulkan Video where supported. |
| macOS Apple Silicon | MoltenVK buffer/image baseline and direct shared-buffer conversion | Controlled host imports, Metal/IOSurface/CoreVideo surfaces, VideoToolbox integration. |
| macOS Intel/AMD | Independently probe the common feature floor and transfers | Shared/managed/private behavior and external surfaces must be measured on those GPUs; do not apply Apple Silicon results to discrete Macs. |

### NVIDIA: optional CUDA without OpenGL or DVP

CUDA documents importing Vulkan-exported memory and synchronization objects. Match CUDA/Vulkan device UUIDs, use the
correct platform handle types, allocation sizes/offsets and dedicated-allocation flags, and matching image descriptions.
Preserve handle ownership and destruction rules. Registration and mapping belong to pool creation, not every frame.
[NVIDIA CUDA interoperability guide](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html)

Prototype exportable Vulkan buffers first: CUDA pinned host memory → shared device buffer → Vulkan conversion, and
the reverse for output. Then test compatible shared images if removing a conversion/copy improves the full path.
Use external semaphore handoffs; select timeline or binary primitives according to actual handle/API support.
Do not assume a decoder-owned CUDA allocation is exportable just because a Vulkan-owned allocation can be imported
by CUDA. A device-local CUDA copy into shared storage is a valid alternative to CPU readback.

DVP is outside the candidate set and is removed entirely during the Linux rewrite. Delete its backend and factory
branches, startup/shutdown calls, wrapper target/linkage, and obsolete instructions. Remove vendored DVP-only material
where tracked and no longer needed; do not modify unrelated locally installed SDKs. No DVP/CUDA bridge or DVP benchmark
is required. Compare native Vulkan against the working non-DVP OpenGL/CUDA paths for the local baseline.

### macOS: shared buffers and native media surfaces

Apple distinguishes CPU/GPU shared resources from GPU-private storage; private resources can enable optimizations that
shared storage cannot. Unified physical memory does not remove all layout/conversion work or synchronization.
[Apple resource fundamentals](https://developer.apple.com/documentation/metal/resource-fundamentals)

Use MoltenVK allocations for the ordinary renderer. For media interop, prefer standard Vulkan extension entry points:
`VK_EXT_external_memory_metal` imports/exports Metal resource handles;
`VK_EXT_metal_objects` additionally exposes image/IOSurface and shared-event integration. Keep the bridge in small
Objective-C++ implementation files on the same underlying Metal device.
[Metal external memory](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_metal.html),
[Metal objects](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_objects.html)

During the later Mac pass, for future VideoToolbox frames, prototype retained `CVPixelBuffer` planes → `CVMetalTextureCache` textures → Vulkan
imports, or a supported IOSurface import. CoreVideo requires retaining the texture wrapper until GPU use finishes.
[CoreVideo texture mapping](https://developer.apple.com/documentation/corevideo/cvmetaltexturecachecreatetexturefromimage(_:_:_:_:_:_:_:_:_:))

Prove plane formats/usages, row layout, decoder completion, Vulkan/Metal visibility, and return-to-producer lifetime.
Where native shared-event synchronization cannot represent an SDK handoff, use its documented completion mechanism
on a worker before publication. An IOSurface or Metal handle alone is not a completion signal. Use a same-device GPU
conversion/copy when a surface cannot be sampled or exported in the required form, before considering a CPU fallback.

This is an interop specialization, not a parallel Metal compositor. For existing SDI/NDI host paths, shared buffers
may already be the fastest implementation and should not be wrapped in IOSurface merely because it is available.

### NDI: optimize the format as well as memory movement

NDI recommends fastest-format receive and UYVY/UYVA sending where possible, including GPU conversion before download.
The current BGRA/BGRX receive and RGBA send paths therefore deserve an end-to-end comparison.
[NDI performance guidance](https://docs.ndi.video/all/developing-with-ndi/sdk/performance-and-implementation)

Implement the formats actually returned by the selected SDK mode, including alpha-bearing variants, before requesting
it. Preserve color range/chroma/alpha metadata and compare both input and output quality; do not force YUV on a path
where it reduces required precision or mishandles keying. Keep explicit RGB capability as a correct alternative.

The NDI Advanced SDK documents custom receive allocators; availability under the project's installed SDK and licensing
has not been established. Treat that as an optional candidate, not a baseline dependency. An SDK accepting custom host
allocations is also not proof that it accepts native GPU surfaces.
[NDI custom allocators](https://docs.ndi.video/all/developing-with-ndi/advanced-sdk/ndi-sdk-review/receiving/custom-allocators)

## Future codec surfaces

Integrate through FFmpeg hardware-frame contexts where possible rather than implementing several decoders ourselves.
Keep frames native until one conversion into the working representation is needed, and keep the original decoder
reference alive through consumption. Make format/codec/profile/bit-depth support part of selection.

| Candidate | Integration proof required |
| --- | --- |
| Vulkan Video/FFmpeg Vulkan frames | Share or adopt the application device/queues; query the exact video profile and format usages. Respect frame layout, access, semaphore values, and FFmpeg locking. |
| NVIDIA NVDEC/NVENC | CUDA-native frames and shared Vulkan storage with explicit interop; GPU-local copy if direct mapping is unavailable. Encoder input format and pitch must match the codec SDK. |
| Windows D3D11/D3D12 media | Shareable handles on the same adapter plus a supported fence/keyed-mutex handoff. A decoder texture is not automatically shareable; use a GPU copy to a compatible shared allocation if needed. |
| Linux VA-API/DRM media | Exportable DMA-BUF planes with exact DRM modifiers, offsets/strides, matching import usages, and explicit producer-completion/ownership handling. |
| macOS VideoToolbox | Retained CoreVideo/IOSurface/Metal planes with the bridge above; choose a compatible output pool for future encoding. |

FFmpeg's Vulkan frame API carries image layouts and timeline-semaphore state that applications must respect; sharing
only its `VkImage` handles is insufficient. Pin an FFmpeg release and implement against that API rather than assume
trunk documentation matches the installed version.
[FFmpeg Vulkan frame context](https://www.ffmpeg.org/doxygen/trunk/hwcontext__vulkan_8h_source.html)

Windows handle types and Linux modifier descriptions are concrete interoperability contracts, not interchangeable
opaque pointers. Query every intended usage and handle type.
[External handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html),
[DRM modifier extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html)

Vulkan Video is optional; the renderer must not depend on codec extensions being available in MoltenVK or on every
native driver. Software decode/encode plus bounded host transfer remains functional fallback, with a visible reason.
Audio/timing/seek/loop behavior belongs to the existing media plan, not the graphics backend.

## Selection, fallback, and ownership

Filter candidates by build availability, device/format/handle capabilities, successful resource creation, SDK allocation
rules, and correctness tests. Select from validated performance profiles for the requested direction, dimensions,
format, memory access, and concurrent workload. Start with measured profiles and an explicit diagnostic override;
add bounded idle-time calibration only where profiles cannot predict the winner. Never benchmark alternatives inside
a live frame deadline or silently vary path every frame.

Cache by device/driver, OS/MoltenVK/SDK versions, transfer request, and relevant workload class. Invalidate on change.
Do not apply a Quadro-derived profile solely because another GPU reports NVIDIA or runs Linux. An unmatched device
uses a capability-correct, explicitly uncalibrated default until suitable measurements exist; it does not inherit a
claim of optimal performance from this machine.
Prefer the simpler path when measured differences are within noise. Log the chosen storage/conversion path, actual
copy count, memory footprint, rejected candidates, and reason for fallback. A stable stream plan keeps pool layout and
conversion consistent; changing it requires retiring/rebuilding the affected pool off the render thread.

| Phase | Ownership rule |
| --- | --- |
| Producer writing/DMA | Neither Vulkan nor another producer accesses the slot. Honor SDK access/completion rules. |
| Submitted input | Freeze bytes and retain allocation; complete CPU cache maintenance and record GPU dependencies. |
| Rendering | Wait on producer dependencies; track every consumer, including repeat use across program frames. |
| Output GPU work | Finish packing/copying and host visibility before publishing a readable lease. |
| External consumer | SDK retains the slot through scheduled-frame completion or asynchronous-send completion. |
| Recycle/retire | All GPU dependencies and external holders are finished; only then overwrite, unregister, or free. |

The DeckLink custom-buffer access cycle and scheduled-output retention are especially important. Its allocator API
provides application-controlled buffers, but the transfer allocation still has to satisfy the SDK's real requirements.
[DeckLink allocator interface](https://sdk-doc.blackmagicdesign.com/decklink-sdk/DeckLinkAPI/interfaces/idecklinkvideobufferallocator.html)

Count allocations, imported storage, padding, mipmaps, decoder-retained surfaces, and deferred destruction in budgets;
do not count one shared allocation twice. Imported memory has a retention cost even if Miximus did not allocate it.
On failures before submission, discard the candidate safely and try the next. After submission, never reuse storage
until completion is established. Device loss fails/cancels affected streams and initiates controlled teardown/recovery;
it must not masquerade as successful transfer completion or endless timeline waiting.
