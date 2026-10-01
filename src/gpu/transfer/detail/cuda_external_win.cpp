#include "cuda_external.hpp"

namespace miximus::gpu::transfer::detail {

cudaError_t
import_external_memory(cudaExternalMemory_t* memory, const gpu::detail::native_handle_s& original, size_t bytes)
{
    auto handle = gpu::detail::native_handle_s::duplicate(original.get());

    cudaExternalMemoryHandleDesc desc{};
    desc.type                = cudaExternalMemoryHandleTypeOpaqueWin32;
    desc.handle.win32.handle = handle.get();
    desc.size                = bytes;
    desc.flags               = cudaExternalMemoryDedicated;

    // CUDA retains its own reference; the duplicate HANDLE closes here.

    return cudaImportExternalMemory(memory, &desc);
}

cudaError_t import_external_semaphore(cudaExternalSemaphore_t* semaphore, gpu::detail::native_handle_s handle)
{
    cudaExternalSemaphoreHandleDesc desc{};
    desc.type                = cudaExternalSemaphoreHandleTypeOpaqueWin32;
    desc.handle.win32.handle = handle.get();

    // CUDA retains its own reference; the exported HANDLE closes here.

    return cudaImportExternalSemaphore(semaphore, &desc);
}

} // namespace miximus::gpu::transfer::detail
