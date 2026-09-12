// Shared runtime defaults for sanitizer builds of GPU-linked executables.
// ASan options are consumed before main(), before --disable-cuda can be parsed.

#ifdef MIXIMUS_SANITIZE_ADDRESS
#include <cstdlib>

#ifdef __linux__
namespace {

// Vulkan Loader's sanitizer support retains driver/layer libraries so LSan can
// inspect their global roots and symbolize their allocation stacks at exit.
// Without this, NVIDIA's process-lifetime DBus allocations appear as leaks from
// already-unloaded modules. Preserve an explicit override for diagnostic runs.
[[maybe_unused]] const bool retained_vulkan_libraries = [] {
    return setenv("VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING", "1", 0) == 0;
}();
} // namespace

#endif

extern "C" const char* __asan_default_options()
{
    return
#ifdef MIXIMUS_CUDA_ASAN_COMPAT
        // CUDA unified virtual addressing still conflicts with ASan's guarded
        // shadow gap with Vulkan interop. Disable that guard, not instrumentation
        // or leak detection. CUDA-free builds keep ASan's default protection.
        "protect_shadow_gap=0:"
#endif
#ifdef MIXIMUS_SANITIZER_ADDR2LINE
        // Keep symbolization without the llvm-symbolizer exit-time deadlock
        // previously observed after NVIDIA driver use.
        "external_symbolizer_path=" MIXIMUS_SANITIZER_ADDR2LINE ":allow_addr2line=1:"
#endif
        "";
}

#ifdef __linux__
extern "C" const char* __lsan_default_suppressions()
{
    // NVIDIA's Vulkan ICD uses these shared driver libraries too. Presentation
    // still leaves allocations behind with library retention enabled. Match the
    // driver frames, not XCB or DBus, so other callers' leaks remain visible.
    return "leak:libnvidia-glcore.so\n"
           "leak:libGLX_nvidia.so\n";
}

#endif
#endif

#ifdef MIXIMUS_SANITIZE_UNDEFINED
extern "C" const char* __ubsan_default_options() { return "print_stacktrace=1:halt_on_error=1"; }
#endif
