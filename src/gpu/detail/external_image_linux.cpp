#include "device.hpp"
#include "external_image.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace miximus::gpu::detail {

std::shared_ptr<texture_state_s> import_external_image(const std::shared_ptr<device_state_s>& device,
                                                       const external_image_s&                descriptor)
{
    if (!device || device->external_image_import != external_image_import_support_e::supported) {
        throw std::runtime_error("DMA-BUF image import is not enabled");
    }

    if (descriptor.handle < 0 || descriptor.extent.width == 0 || descriptor.extent.height == 0 ||
        descriptor.extent.width > device->properties.limits.maxImageDimension2D ||
        descriptor.extent.height > device->properties.limits.maxImageDimension2D ||
        descriptor.stride < uint64_t{descriptor.extent.width} * 4 ||
        descriptor.stride > (std::numeric_limits<uint64_t>::max() - descriptor.offset) / descriptor.extent.height) {
        throw std::invalid_argument("Invalid DMA-BUF image layout");
    }

    VkFormat format{};
    switch (descriptor.order) {
        case channel_order_e::rgba:
            format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case channel_order_e::bgra:
            format = VK_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            throw std::invalid_argument("Unsupported DMA-BUF image channel order");
    }

    VkDrmFormatModifierPropertiesListEXT modifiers{
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };

    VkFormatProperties2 formats{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifiers,
    };

    device->instance_vk.vkGetPhysicalDeviceFormatProperties2(device->physical, format, &formats);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = entries.data();

    device->instance_vk.vkGetPhysicalDeviceFormatProperties2(device->physical, format, &formats);
    entries.resize(modifiers.drmFormatModifierCount);
    const auto found =
        std::ranges::find(entries, descriptor.modifier, &VkDrmFormatModifierPropertiesEXT::drmFormatModifier);
    constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (found == entries.end() || found->drmFormatModifierPlaneCount != 1 ||
        (found->drmFormatModifierTilingFeatures & required) != required) {
        throw std::runtime_error("DMA-BUF modifier does not support single-plane filtered sampling");
    }

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query{
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = descriptor.modifier,
        .sharingMode       = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkPhysicalDeviceExternalImageFormatInfo external_query{
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext      = &modifier_query,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkPhysicalDeviceImageFormatInfo2 query{
        .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext  = &external_query,
        .format = format,
        .type   = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT,
    };

    VkExternalImageFormatProperties external_properties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };

    VkImageFormatProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };

    check(device->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device->physical, &query, &properties),
          "query DMA-BUF image format");
    if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0 ||
        descriptor.extent.width > properties.imageFormatProperties.maxExtent.width ||
        descriptor.extent.height > properties.imageFormatProperties.maxExtent.height) {
        throw std::runtime_error("DMA-BUF image format/extent is not importable");
    }

    auto image      = std::make_shared<texture_state_s>();
    image->owner    = device;
    image->extent   = descriptor.extent;
    image->format   = format_e::rgba_unorm8;
    image->sampling = sampling_e::linear;
    // Imported contents must be preserved; a future ownership-acquire helper
    // must seed its recording layout before the usual submission prologue.
    image->layouts = {VK_IMAGE_LAYOUT_GENERAL};

    VkSubresourceLayout plane{
        .offset   = descriptor.offset,
        .rowPitch = descriptor.stride,
    };

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
        .sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier           = descriptor.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts               = &plane,
    };

    VkExternalMemoryImageCreateInfo external{
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext       = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo info{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = &external,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = format,
        .extent      = {.width = descriptor.extent.width, .height = descriptor.extent.height, .depth = 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = query.tiling,
        .usage       = query.usage,
    };

    check(device->vk.vkCreateImage(device->device, &info, nullptr, &image->image), "create DMA-BUF image");

    VkMemoryRequirements requirements{};

    device->vk.vkGetImageMemoryRequirements(device->device, image->image, &requirements);

    VkMemoryFdPropertiesKHR fd_properties{
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };

    check(device->vk.vkGetMemoryFdPropertiesKHR(
              device->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, descriptor.handle, &fd_properties),
          "query DMA-BUF memory types");
    const auto memory_types = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    if (memory_types == 0) {
        throw std::runtime_error("DMA-BUF has no compatible Vulkan memory type");
    }

    // Only the duplicate is transferred to Vulkan, and only on allocation success.
    auto duplicate = native_handle_s::duplicate(descriptor.handle);

    VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image->image,
    };

    VkImportMemoryFdInfoKHR import{
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext      = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd         = duplicate.get(),
    };

    VkMemoryAllocateInfo allocation{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &import,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = static_cast<uint32_t>(std::countr_zero(memory_types)),
    };
    const auto result = device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &image->external_memory);
    if (result != VK_SUCCESS) {
        check(result, "import DMA-BUF memory");
    }

    (void)duplicate.release();
    image->external_allocation_bytes = requirements.size;

    check(device->vk.vkBindImageMemory(device->device, image->image, image->external_memory, 0), "bind DMA-BUF image");

    VkImageViewCreateInfo view{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel   = 0,
                             .levelCount     = 1,
                             .baseArrayLayer = 0,
                             .layerCount     = 1},
    };

    check(device->vk.vkCreateImageView(device->device, &view, nullptr, &image->view), "create DMA-BUF sampled view");
    image->sampled_view = image->view;
    return image;
}

} // namespace miximus::gpu::detail
