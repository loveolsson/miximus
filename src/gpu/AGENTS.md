# GPU subsystem instructions

Read [../../docs/gpu-and-media.md](../../docs/gpu-and-media.md) before changing this subtree.

- OpenGL context ownership is explicit. Use `context_scope_s` whenever making a context current.
- GL resources must be created and destroyed with an appropriate context current. A context is owned by one native thread at a time; share GL objects through parent/worker contexts rather than moving one context between threads.
- Do not add node-specific PBO or readback implementations; extend the internal transfer backend interface when a service needs a new transfer mode.
- Preserve transfer priority: DVP, CUDA/OpenGL interop, persistent PBO fallback.
- Bind each backend instance to one slot texture for its lifetime. Pair unbinding with destruction and preserve GL/DVP ownership transitions.
- GPU-to-CPU host memory is not ready for workers until backend completion succeeds.
- CUDA transfers use an OpenGL pixel buffer when GL format conversion is required. Raw CUDA-array copies are only safe for formats explicitly proven storage-identical.
- Transfer backend initialization and shutdown require the root GL context current. Keep teardown before root-context destruction and after transfer resources are gone.
- Keep native texture external format/type, storage format, dimensions, and byte size consistent when adding formats.
- Build success does not validate DVP, CUDA, GL synchronization, or display timing; test on appropriate hardware.
