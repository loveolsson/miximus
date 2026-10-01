#pragma once

#include "utils/failure_shutdown.hpp"

#include <string_view>

namespace miximus::gpu::detail {

// Preserve uncertain GPU ownership while the main thread shuts down and the
// independent recovery worker saves settings. Only the watchdog forces exit.
[[noreturn]] inline void fatal_gpu_error(std::string_view message, std::string_view detail = {}) noexcept
{
    utils::fail_without_unwinding(message, detail);
}

} // namespace miximus::gpu::detail
