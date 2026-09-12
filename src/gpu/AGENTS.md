# GPU subsystem instructions

Read [../../docs/gpu-and-media.md](../../docs/gpu-and-media.md) before changing this subtree.

- Keep Vulkan, Volk, VMA, and platform handles in implementation headers. Ordinary node-facing APIs expose typed images, buffers, draw parameters, and completion tokens.
- Third-party source belongs in pinned Git submodules under `submodules/`, with dependency discovery and integration under `src/wrapper/`.
- Record image/buffer use centrally. Commit layouts and last-use timeline values only after successful submission; aborted recordings must not publish unsignalled values.
- Give the graph, transfer services, and presenters independent recording contexts. Bound in-flight recordings and stream/host memory pools; grow and reuse descriptor pages as draw demand increases. Busy workers retry; exhausted render capacity drops the unfinished evaluation.
- Retain GPU resources through actual completion and host buffers through every external lease. A CPU lifecycle callback or mutex is not GPU completion.
- Preserve the private transfer-backend contract and DeckLink's direct access to backend-owned host memory. Future DVP support must fit the same leases without an SDK-to-staging CPU copy. See [the direct-memory contract](../../docs/decklink-direct-memory.md).
- Flush noncoherent producer writes before submission and invalidate readback memory after completion, before publication.
- Producers enqueue uploads before publishing frames to the timed FIFO. Preserve exact PTS-selected upload IDs and wait at FIFO consumption after buffering; never replace PTS selection with readiness-based selection. Retain submitted FIFO upload leases until consumption or eviction.
- Enqueue command bodies without waiting for other recorders. The submission worker resolves initial resource layouts in submission order and publishes outputs only after successful native submission.
- Keep GLFW window, monitor, and event operations on the main thread; presentation acquires independently of graph recording.
- Keep GPU completion, presentation semaphore consumption, and display timing distinct.
- Preserve explicit host format, row stride, alignment, component mapping, UNORM precision, and premultiplied alpha contracts.
- Keep transfer storage directly shareable: RGBA/BGRA/BGRX/ARGB bytes use RGBA8 images with shader swizzling; v210 uses raw 32-bit words with shader packing. CUDA imports the actual frame resource, never an intermediate device frame. New formats must preserve this design; startup must qualify every required representation and disable CUDA for the entire run if any fails; selection is strict once chosen; `--disable-cuda` forces staging.
- Run both GPU test executables with synchronization validation and exercise native Wayland, DeckLink, and NDI separately from ordinary CTest.
- Format touched C++ and run the full native build and `git diff --check`.
