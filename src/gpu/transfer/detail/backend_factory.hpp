#pragma once
#include "backend.hpp"

#include <memory>

namespace miximus::gpu::transfer::detail {

void initialize_backends();
void shutdown_backends();

std::unique_ptr<backend_i> create_backend(size_t size, backend_i::direction_e direction);

} // namespace miximus::gpu::transfer::detail
