#pragma once

#include "native_handle.hpp"

#include <volk.h>

namespace miximus::gpu::detail {

struct device_state_s;

extern const VkExternalMemoryHandleTypeFlagBits    opaque_memory_handle_type;
extern const VkExternalSemaphoreHandleTypeFlagBits cuda_semaphore_handle_type;
extern const char* const                           opaque_memory_extension;
extern const char* const                           cuda_semaphore_extension;

// Dedicated opaque memory for external consumers. The caller selects the memory type.
VkDeviceMemory allocate_external_memory(device_state_s& device,
                                        VkDeviceSize    bytes,
                                        uint32_t        memory_type,
                                        VkImage         image,
                                        VkBuffer        buffer);

native_handle_s export_memory_handle(device_state_s& device, VkDeviceMemory memory);
native_handle_s export_semaphore_handle(device_state_s& device, VkSemaphore semaphore);

// Same-driver Vulkan interop. Import duplicates borrowed handles; successful FD
// imports transfer ownership to Vulkan. No native handles escape implementation APIs.
native_handle_s export_opaque_memory(const VolkDeviceTable& vk, VkDevice device, VkDeviceMemory memory);
VkDeviceMemory  import_opaque_buffer_memory(const VolkDeviceTable&   vk,
                                            VkDevice                 device,
                                            VkBuffer                 buffer,
                                            size_t                   bytes,
                                            uint32_t                 memory_type,
                                            native_handle_s::value_t borrowed);

} // namespace miximus::gpu::detail
