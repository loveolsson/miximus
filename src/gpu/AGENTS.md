# GPU subsystem instructions

Read [../../docs/gpu-and-media.md](../../docs/gpu-and-media.md) before changing this subtree.

- OpenGL context ownership is explicit. Use `context_scope_s` whenever making a context current.
- GL resources must be created and destroyed with an appropriate context current. Pass `context_lock_e::lock` to the scope for shared contexts.
- Do not add node-specific PBO or readback implementations when `transfer_i` can express the operation.
- Preserve transfer priority: DVP, CUDA/OpenGL interop, persistent PBO fallback.
- Use the transfer instance's virtual `type()` for lifetime hooks.
- Pair DVP texture registration/unregistration and preserve GL/DVP ownership transitions.
- GPU-to-CPU host memory is not ready for workers until `wait_for_copy()` completes.
- CUDA transfers intentionally interoperate through an OpenGL pixel buffer so GL performs format conversion. Raw CUDA-array copies are unsafe when host and texture storage formats differ.
- Transfer backend initialization and shutdown require the root GL context current. Keep teardown before root-context destruction and after transfer resources are gone.
- Keep native texture external format/type, storage format, dimensions, and byte size consistent when adding formats.
- Build success does not validate DVP, CUDA, GL synchronization, or display timing; test on appropriate hardware.
