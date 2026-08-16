#pragma once
#include "gpu/transfer/texture_transfer.hpp"
#include "texture_transfer_backend.hpp"

#include <cstdint>
#include <memory>

namespace miximus::gpu::transfer::detail {

void initialize_texture_transfer_backends();
void shutdown_texture_transfer_backends();

enum class texture_transfer_backend_kind_e : std::uint8_t
{
    persistent,
    cuda,
    dvp,
};

enum class texture_transfer_memory_path_e : std::uint8_t
{
    pixel_buffer,
    direct_image,
    direct_memory,
};

struct texture_transfer_backend_selection_s
{
    std::unique_ptr<texture_transfer_backend_i> transfer_backend;
    texture_transfer_backend_kind_e             backend_kind;
    texture_transfer_memory_path_e              memory_path;
    size_t                                      backend_allocation_bytes;
};

texture_transfer_backend_selection_s create_texture_transfer_backend(const texture_transfer_layout_s& transfer_layout,
                                                                     texture_transfer_backend_i::direction_e direction,
                                                                     texture_s*                              texture);

} // namespace miximus::gpu::transfer::detail
