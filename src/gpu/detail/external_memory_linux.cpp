#include "device.hpp"
#include "external_memory.hpp"

namespace miximus::gpu::detail {

const VkExternalMemoryHandleTypeFlagBits    cuda_memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
const VkExternalSemaphoreHandleTypeFlagBits cuda_semaphore_handle_type =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
const char* const cuda_memory_extension    = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
const char* const cuda_semaphore_extension = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;

VkDeviceMemory allocate_external_memory(device_state_s& device,
                                        VkDeviceSize    bytes,
                                        uint32_t        memory_type,
                                        VkImage         image,
                                        VkBuffer        buffer)
{
    VkMemoryDedicatedAllocateInfo dedicated{
        .sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image  = image,
        .buffer = buffer,
    };

    VkExportMemoryAllocateInfo export_info{
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext       = &dedicated,
        .handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(cuda_memory_handle_type),
    };

    VkMemoryAllocateInfo allocate{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &export_info,
        .allocationSize  = bytes,
        .memoryTypeIndex = memory_type,
    };

    VkDeviceMemory memory{};
    check(device.vk.vkAllocateMemory(device.device, &allocate, nullptr, &memory), "allocate CUDA shared resource");

    return memory;
}

native_handle_s export_memory_handle(device_state_s& device, VkDeviceMemory memory)
{
    VkMemoryGetFdInfoKHR info{
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory     = memory,
        .handleType = cuda_memory_handle_type,
    };

    native_handle_s::value_t value{-1};
    check(device.vk.vkGetMemoryFdKHR(device.device, &info, &value), "export Vulkan memory handle");

    return native_handle_s(value);
}

native_handle_s export_semaphore_handle(device_state_s& device, VkSemaphore semaphore)
{
    VkSemaphoreGetFdInfoKHR info{
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore  = semaphore,
        .handleType = cuda_semaphore_handle_type,
    };

    native_handle_s::value_t value{-1};
    check(device.vk.vkGetSemaphoreFdKHR(device.device, &info, &value), "export Vulkan semaphore handle");

    return native_handle_s(value);
}

} // namespace miximus::gpu::detail
