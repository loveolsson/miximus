// Runtime defaults for sanitizer builds. These hooks are consumed directly by
// the sanitizer runtimes before main() starts.

#ifdef MIXIMUS_SANITIZE_ADDRESS
extern "C" const char* __asan_default_options()
{
    // CUDA unified virtual addressing needs to map addresses in ASan's unused
    // shadow gap. The gap remains instrumented; only its guard is disabled.
#ifdef MIXIMUS_SANITIZER_ADDR2LINE
    // Clang's external llvm-symbolizer protocol can deadlock during LSan's
    // exit-time scan after the NVIDIA driver has been used. Compiler-rt also
    // supports the addr2line protocol, which retains online symbolization
    // without requiring environment variables when running Miximus.
    return "protect_shadow_gap=0:external_symbolizer_path=" MIXIMUS_SANITIZER_ADDR2LINE ":allow_addr2line=1";
#else
    return "protect_shadow_gap=0";
#endif
}

extern "C" const char* __lsan_default_suppressions()
{
    // NVIDIA's proprietary OpenGL driver retains process-lifetime allocations,
    // including allocations made through DBus. Keep all other leaks reportable.
    return "leak:libnvidia-glcore.so\n";
}
#endif

#ifdef MIXIMUS_SANITIZE_UNDEFINED
extern "C" const char* __ubsan_default_options() { return "print_stacktrace=1:halt_on_error=1"; }
#endif
