#include "cuda_external.hpp"

namespace miximus::gpu::transfer::detail {

cudaError_t
import_external_memory(cudaExternalMemory_t* memory, const gpu::detail::native_handle_s& original, size_t bytes)
{
    auto handle = gpu::detail::native_handle_s::duplicate(original.get());

    cudaExternalMemoryHandleDesc desc{};
    desc.type      = cudaExternalMemoryHandleTypeOpaqueFd;
    desc.handle.fd = handle.get();
    desc.size      = bytes;
    desc.flags     = cudaExternalMemoryDedicated;

    const auto result = cudaImportExternalMemory(memory, &desc);

    // CUDA takes ownership of the FD only on successful import.
    if (result == cudaSuccess) {
        (void)handle.release();
    }

    return result;
}

cudaError_t import_external_semaphore(cudaExternalSemaphore_t* semaphore, gpu::detail::native_handle_s handle)
{
    cudaExternalSemaphoreHandleDesc desc{};
    desc.type      = cudaExternalSemaphoreHandleTypeOpaqueFd;
    desc.handle.fd = handle.get();

    const auto result = cudaImportExternalSemaphore(semaphore, &desc);

    // CUDA takes ownership of the FD only on successful import.
    if (result == cudaSuccess) {
        (void)handle.release();
    }

    return result;
}

} // namespace miximus::gpu::transfer::detail
