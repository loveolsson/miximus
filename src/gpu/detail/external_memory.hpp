#pragma once

#include "native_handle.hpp"

#include <volk.h>

namespace miximus::gpu::detail {

struct device_state_s;

extern const VkExternalMemoryHandleTypeFlagBits    cuda_memory_handle_type;
extern const VkExternalSemaphoreHandleTypeFlagBits cuda_semaphore_handle_type;
extern const char* const                           cuda_memory_extension;
extern const char* const                           cuda_semaphore_extension;

// Dedicated opaque memory for external consumers. The caller selects the memory type.
VkDeviceMemory allocate_external_memory(device_state_s& device,
                                        VkDeviceSize    bytes,
                                        uint32_t        memory_type,
                                        VkImage         image,
                                        VkBuffer        buffer);

native_handle_s export_memory_handle(device_state_s& device, VkDeviceMemory memory);
native_handle_s export_semaphore_handle(device_state_s& device, VkSemaphore semaphore);

} // namespace miximus::gpu::detail
