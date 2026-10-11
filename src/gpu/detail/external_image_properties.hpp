#pragma once

#include "device.hpp"

#include <vector>

namespace miximus::gpu::detail {

struct external_image_properties_s
{
    VkResult                   result;
    VkImageFormatProperties    image;
    VkExternalMemoryProperties memory;
};

// Callers retain import/export policy and platform-specific query extensions.
inline external_image_properties_s query_external_image_properties(const device_state_s&              device,
                                                                   VkFormat                           format,
                                                                   VkImageTiling                      tiling,
                                                                   VkImageUsageFlags                  usage,
                                                                   VkExternalMemoryHandleTypeFlagBits handle_type,
                                                                   const void* platform_query = nullptr)
{
    const VkPhysicalDeviceExternalImageFormatInfo external{
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext      = platform_query,
        .handleType = handle_type,
    };
    const VkPhysicalDeviceImageFormatInfo2 query{
        .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext  = &external,
        .format = format,
        .type   = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage  = usage,
    };
    VkExternalImageFormatProperties memory{.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2        properties{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &memory};
    const auto                      result =
        device.instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device.physical, &query, &properties);
    return {.result = result, .image = properties.imageFormatProperties, .memory = memory.externalMemoryProperties};
}

inline std::vector<VkDrmFormatModifierPropertiesEXT> image_modifiers(const device_state_s& device, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT modifiers{.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &modifiers};
    device.instance_vk.vkGetPhysicalDeviceFormatProperties2(device.physical, format, &properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = entries.data();
    device.instance_vk.vkGetPhysicalDeviceFormatProperties2(device.physical, format, &properties);
    entries.resize(modifiers.drmFormatModifierCount);
    return entries;
}
} // namespace miximus::gpu::detail
