#pragma once

#include "gpu/transfer/texture_transfer.hpp"

namespace miximus::gpu::transfer::detail {

void   normalize_transfer_layout(texture_transfer_layout_s& transfer_layout);
size_t estimate_slot_memory_usage(const texture_transfer_layout_s& transfer_layout);
size_t slot_memory_usage(const texture_transfer_layout_s& transfer_layout, size_t backend_allocation_bytes);

} // namespace miximus::gpu::transfer::detail
