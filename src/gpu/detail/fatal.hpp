#pragma once

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace miximus::gpu::detail {

// GPU ownership/completion is no longer trustworthy. Do not unwind through
// resource retirement or attempt to reuse the device after this point.
[[noreturn]] inline void fatal_gpu_error(std::string_view message) noexcept
{
    std::fputs("Fatal GPU error: ", stderr);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

} // namespace miximus::gpu::detail
