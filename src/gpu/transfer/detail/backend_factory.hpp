#pragma once
#include "backend.hpp"
#include "gpu/transfer/texture_transfer.hpp"

#include <cstdint>
#include <memory>

namespace miximus::gpu::transfer::detail {

void initialize_backends();
void shutdown_backends();

enum class backend_type_e : std::uint8_t
{
    persistent,
    cuda,
    dvp,
};

enum class transfer_path_e : std::uint8_t
{
    pixel_buffer,
    direct_image,
    direct_memory,
};

struct backend_result_s
{
    std::unique_ptr<backend_i> backend;
    backend_type_e             type;
    transfer_path_e            path;
    size_t                     allocation_bytes;
};

backend_result_s create_backend(const texture_transfer_requirements_s& requirements,
                                backend_i::direction_e                 direction,
                                texture_s*                             texture);

} // namespace miximus::gpu::transfer::detail
