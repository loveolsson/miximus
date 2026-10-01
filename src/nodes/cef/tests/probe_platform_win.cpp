#include "probe_platform.hpp"

#include <Windows.h>
#include <iostream>

namespace miximus::nodes::cef::tests {

void log_producer_fence(gpu::detail::native_handle_s::value_t handle)
{
    // CEF's Windows callback exposes the texture, not a queryable producer fence.
    std::cout << "CEF shared texture handle: " << handle << '\n';
}

void configure_crash_probe() { SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX); }

} // namespace miximus::nodes::cef::tests
