# DeckLink direct-memory transfer contract

DVP implementation removal does **not** remove its architectural requirements. DeckLink must continue to DMA directly
into/from the transfer backend's host allocation, without an intermediate CPU frame copy. CUDA remains a supported
backend requested with `--use-cuda` and selected after startup qualification; one P2000's measurements are not a reason to remove it or infer performance on other
machines.

## Reviewed sources

The repository's DeckLink SDK 16.0 includes the original model under
`3rd-party/decklink-sdk/Linux/Samples/LoopThroughWithOpenGLCompositing/`:

- [OpenGLComposite.cpp](../3rd-party/decklink-sdk/Linux/Samples/LoopThroughWithOpenGLCompositing/OpenGLComposite.cpp): `PinnedMemoryAllocator` caches aligned addresses, retains a `VideoFrameTransfer` registration per address, destroys registration before freeing that address, and exposes it through `DeckLinkVideoBuffer::GetBytes()`.
- [VideoFrameTransfer.cpp](../3rd-party/decklink-sdk/Linux/Samples/LoopThroughWithOpenGLCompositing/VideoFrameTransfer.cpp): pins/registers the supplied host address, maintains synchronization objects, transfers in both directions, separates graphics ownership from transfer completion, and unregisters before unpinning. Input and output have different packed layouts.
- The pre-rewrite integration remains available with `git show 0047a63:src/gpu/transfer/detail/dvp.cpp` and the corresponding `texture_transfer_backend.hpp` and factory files.

These are references for allocation, registration, ownership and synchronization. Their GL calls are not a Vulkan
implementation. The installed DVP package exposes GL/CUDA interfaces; a future implementation still needs a supported,
tested interoperability path to the app's Vulkan resources. No DVP runtime dependency or untested DVP implementation is
restored here, and the SDK's source/headers are not copied into first-party code.

## Requirements retained in the application

1. **Backend-owned host memory.** The transfer backend chooses its allocator and may pin/register that allocation. The SDK receives that exact address through an upload or readback lease. CUDA-pinned memory, Vulkan-mapped memory, and a future DVP allocation use the same SDK-facing contract. CUDA also transfers directly into/from the actual Vulkan byte image or raw-word buffer, with no intermediate GPU frame.
2. **Stable registration identity.** An allocation's address, extent and layout stay stable for the slot's lifetime; registration can be cached per slot. A reusable SDK COM buffer can acquire different slots across capture cycles. Registration follows the allocation, not the COM object's address.
3. **Exact host layout.** Pixel format, SDK row stride (including v210 padding), byte size, address alignment and memory access are explicit. Backends may use stronger alignment or larger private allocations and must account for that storage. They must reject incompatible layouts rather than silently repack into a second CPU buffer. The factory validates exposed address, size and requested alignment before publishing a slot. DeckLink output explicitly requests read/write host memory because the SDK can request write access.
4. **Independent SDK and transfer lifetimes.** Input `StartAccess(write)` obtains an exclusive write-cycle lease. DeckLink writes through `GetBytes()`; after capture completes, the callback submits the transfer and moves the same lease into the timed-source FIFO. Rendering waits for the exact PTS-selected upload at FIFO consumption; the FIFO-held lease prevents premature reclamation of later completed uploads. Reusing or releasing the SDK buffer does not revoke that lease. `submit()` transfers access only after the producer finishes. The exact upload ID and retained GPU frame protect against out-of-order delivery and premature reuse.
5. **Completed readback before SDK publication.** Output consumes only completed readback leases. `output_video_buffer_s` moves that lease into the buffer passed to `CreateVideoFrameWithBuffer`. Its `GetBytes()` exposes the same allocation. The lease survives until the last SDK reference releases the buffer, including repeats and playback shutdown; scheduling a frame is not permission to recycle its memory.
6. **Backend-owned GPU hand-offs.** `transfer_backend_i` owns host allocation, registration, transfer submission, visibility and ownership synchronization. Its upload submission ticket must cover the complete hand-off back to graphics, allowing a consumer to queue a GPU dependency before CPU completion reporting. `transfer_ready()` must separately establish actual DMA completion and returned graphics ownership before host reuse or readback publication. A graphics-queue wait alone cannot establish host completion. The existing rendering dependency and per-resource retirement still apply. The CUDA direct-resource backend allocates no Vulkan staging buffer.
7. **Bounded callbacks and teardown.** Allocation/registration/destruction run on a resource worker separate from transfer submission/completion progress. Granting a free lease cannot perform registration, GPU submission or completion waits on an SDK callback. Capture overload uses the existing bounded/drop behavior. Streams drain external leases and submitted resource uses before freeing slots. Backend destruction precedes frame destruction, even during exception cleanup; unregister/unbind precedes unpin/free. Device teardown follows SDK/control-worker and transfer-service shutdown.

`src/gpu/transfer/detail/transfer_backend.hpp` defines the private extension point. The factory in
`transfer_backend.cpp` selects the current Vulkan or CUDA implementation. `frame_staging_s` is the slot facade; its name
does not require a staging-buffer implementation. SDK nodes do not depend on either implementation or on native
Vulkan/CUDA/DVP handles. A future backend must implement the same completion semantics, integrate discovery/linkage
under `src/wrapper/`, and be validated on suitable DVP hardware. Device resource creation may need additional
export/registration capabilities for that implementation; the host lease contract must remain intact.

## Regression checks

`DeckLinkUsesTransferMemoryDirectlyAndRetainsItThroughSdkReferences` exercises the real input allocator and output COM
buffer wrappers with synthetic DMA writes. It checks pointer identity, 4096-byte alignment, padded v210 byte
preservation, two capture cycles on one SDK buffer, pool exhaustion while leases are held, allocator shutdown with an
outstanding upload lease, retained GPU-frame ownership, and output reuse only after the last COM reference is released.
It runs on both Vulkan and CUDA; it does not emulate DVP calls or claim DVP hardware validation.

```bash
build/src/gpu/gpu_transfer_vulkan_test --gtest_filter='*DeckLinkUsesTransferMemory*'
build/src/gpu/gpu_transfer_vulkan_test --use-cuda --gtest_filter='*DeckLinkUsesTransferMemory*'
```

Run the full transfer suite as well; it covers exact upload IDs, abandoned recordings, row strides, concurrent readback
ordering and budgets. Synchronization validation and live DeckLink/NDI runs remain necessary when changing registrations
or GPU ownership behavior. The guard tests require the installed DeckLink SDK but do not open a DeckLink device or
transmit video.

## Validation recorded 2026-09-09

The native build and 104 ordinary tests passed. All nine transfer tests passed with both Vulkan and CUDA, including the
real SDK buffer-wrapper regression; all 22 renderer tests passed. GPU tests used synchronization validation. The backend
factory/facade also compiled with CUDA excluded.

The copied, previously approved DeckLink Duo (2) input / Duo (1) output, NDI and screen graph ran for 30 seconds in each
backend mode with synchronization validation enabled. Both runs logged actual upload and readback completions, started
DeckLink capture/playback and NDI sender/receiver, and shut down cleanly without logged errors or validation messages.
The original saved settings hash remained unchanged. These checks validate the retained memory/lease paths with
current backends; DVP still requires its own implementation and suitable-hardware validation.
