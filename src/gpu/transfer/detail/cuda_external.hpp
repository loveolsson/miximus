#pragma once

#include "gpu/detail/native_handle.hpp"

#include <cstddef>
#include <cuda_runtime_api.h>

namespace miximus::gpu::transfer::detail {

// Import a duplicate, retaining the allocation's original reference. Return CUDA
// errors to the shared transfer code so its fatal-error policy remains authoritative.
cudaError_t
import_external_memory(cudaExternalMemory_t* memory, const gpu::detail::native_handle_s& original, size_t bytes);

// Takes ownership of the freshly exported semaphore handle, including on failure.
cudaError_t import_external_semaphore(cudaExternalSemaphore_t* semaphore, gpu::detail::native_handle_s handle);

} // namespace miximus::gpu::transfer::detail
