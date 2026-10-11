#include "device.hpp"
#include "external_memory.hpp"

#include <dxgi1_2.h>

namespace miximus::gpu::detail {

const VkExternalMemoryHandleTypeFlagBits    opaque_memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
const VkExternalSemaphoreHandleTypeFlagBits cuda_semaphore_handle_type =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
const char* const opaque_memory_extension  = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
const char* const cuda_semaphore_extension = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;

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

    VkExportMemoryWin32HandleInfoKHR win32_export{
        .sType    = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .pNext    = &dedicated,
        .dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
    };
    VkExportMemoryAllocateInfo export_info{
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext       = &win32_export,
        .handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(opaque_memory_handle_type),
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
    return export_opaque_memory(device.vk, device.device, memory);
}

native_handle_s export_opaque_memory(const VolkDeviceTable& vk, VkDevice device, VkDeviceMemory memory)
{
    VkMemoryGetWin32HandleInfoKHR info{
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory     = memory,
        .handleType = opaque_memory_handle_type,
    };

    native_handle_s::value_t value{nullptr};
    check(vk.vkGetMemoryWin32HandleKHR(device, &info, &value), "export Vulkan memory handle");

    return native_handle_s(value);
}

native_handle_s export_semaphore_handle(device_state_s& device, VkSemaphore semaphore)
{
    VkSemaphoreGetWin32HandleInfoKHR info{
        .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
        .semaphore  = semaphore,
        .handleType = cuda_semaphore_handle_type,
    };

    native_handle_s::value_t value{nullptr};
    check(device.vk.vkGetSemaphoreWin32HandleKHR(device.device, &info, &value), "export Vulkan semaphore handle");

    return native_handle_s(value);
}

VkDeviceMemory import_opaque_buffer_memory(const VolkDeviceTable&   vk,
                                           VkDevice                 device,
                                           VkBuffer                 buffer,
                                           size_t                   bytes,
                                           uint32_t                 memory_type,
                                           native_handle_s::value_t borrowed)
{
    auto                             handle = native_handle_s::duplicate(borrowed);
    VkMemoryDedicatedAllocateInfo    dedicated{.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                               .buffer = buffer};
    VkImportMemoryWin32HandleInfoKHR imported{.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                              .pNext      = &dedicated,
                                              .handleType = opaque_memory_handle_type,
                                              .handle     = handle.get()};
    VkMemoryAllocateInfo             allocate{.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                              .pNext           = &imported,
                                              .allocationSize  = bytes,
                                              .memoryTypeIndex = memory_type};
    VkDeviceMemory                   memory{};
    check(vk.vkAllocateMemory(device, &allocate, nullptr, &memory), "import opaque buffer memory");
    return memory;
}

} // namespace miximus::gpu::detail
