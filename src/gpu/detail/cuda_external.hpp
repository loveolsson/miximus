#pragma once

#include <volk.h>

namespace miximus::gpu::detail {

#ifdef _WIN32
inline constexpr auto cuda_memory_handle_type    = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
inline constexpr auto cuda_semaphore_handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
inline constexpr auto cuda_memory_extension      = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
inline constexpr auto cuda_semaphore_extension   = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
#else
inline constexpr auto cuda_memory_handle_type    = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
inline constexpr auto cuda_semaphore_handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
inline constexpr auto cuda_memory_extension      = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
inline constexpr auto cuda_semaphore_extension   = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
#endif

} // namespace miximus::gpu::detail
