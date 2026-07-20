#pragma once

#include "gpu/transfer/texture_transfer.hpp"

namespace miximus::gpu::transfer::detail {

void   normalize_requirements(texture_transfer_requirements_s& requirements);
size_t estimate_slot_memory_usage(const texture_transfer_requirements_s& requirements);
size_t slot_memory_usage(const texture_transfer_requirements_s& requirements, size_t backend_allocation_bytes);

} // namespace miximus::gpu::transfer::detail
