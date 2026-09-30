#pragma once

#include "gpu/detail/native_handle.hpp"

namespace miximus::nodes::cef::tests {

void log_producer_fence(gpu::detail::native_handle_s::value_t handle);
void configure_crash_probe();

} // namespace miximus::nodes::cef::tests
