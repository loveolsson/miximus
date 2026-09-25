#include "dma_buf_image.hpp"

#include "device.hpp"
#include "recording.hpp"
#include "resource.hpp"
#include "utils/owned_fd.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace miximus::gpu::detail {

std::shared_ptr<texture_state_s> import_dma_buf_image(const std::shared_ptr<device_state_s>& device,
                                                      const dma_buf_image_s&                 descriptor)
{
    if (!device || !device->external_image_import.enabled) {
        throw std::runtime_error("DMA-BUF image import is not enabled");
    }

    if (descriptor.fd < 0 || descriptor.extent.width == 0 || descriptor.extent.height == 0 ||
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

    VkDrmFormatModifierPropertiesListEXT modifiers{};
    modifiers.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

    VkFormatProperties2 formats{};
    formats.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    formats.pNext = &modifiers;

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

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query{};
    modifier_query.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier_query.drmFormatModifier = descriptor.modifier;
    modifier_query.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;

    VkPhysicalDeviceExternalImageFormatInfo external_query{};
    external_query.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_query.pNext      = &modifier_query;
    external_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkPhysicalDeviceImageFormatInfo2 query{};
    query.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext  = &external_query;
    query.format = format;
    query.type   = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    query.usage  = VK_IMAGE_USAGE_SAMPLED_BIT;

    VkExternalImageFormatProperties external_properties{};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

    VkImageFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &external_properties;

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

    VkSubresourceLayout plane{};
    plane.offset   = descriptor.offset;
    plane.rowPitch = descriptor.stride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{};
    modifier.sType                       = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier.drmFormatModifier           = descriptor.modifier;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts               = &plane;

    VkExternalMemoryImageCreateInfo external{};
    external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext       = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext       = &external;
    info.imageType   = VK_IMAGE_TYPE_2D;
    info.format      = format;
    info.extent      = {.width = descriptor.extent.width, .height = descriptor.extent.height, .depth = 1};
    info.mipLevels   = 1;
    info.arrayLayers = 1;
    info.samples     = VK_SAMPLE_COUNT_1_BIT;
    info.tiling      = query.tiling;
    info.usage       = query.usage;

    check(device->vk.vkCreateImage(device->device, &info, nullptr, &image->image), "create DMA-BUF image");

    VkMemoryRequirements requirements{};

    device->vk.vkGetImageMemoryRequirements(device->device, image->image, &requirements);

    VkMemoryFdPropertiesKHR fd_properties{};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;

    check(device->vk.vkGetMemoryFdPropertiesKHR(
              device->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, descriptor.fd, &fd_properties),
          "query DMA-BUF memory types");
    const auto memory_types = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    if (memory_types == 0) {
        throw std::runtime_error("DMA-BUF has no compatible Vulkan memory type");
    }

    // Only the duplicate is transferred to Vulkan, and only on allocation success.
    utils::owned_fd_s duplicate{fcntl(descriptor.fd, F_DUPFD_CLOEXEC, 0)};
    if (duplicate.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "duplicate DMA-BUF FD");
    }

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image->image;

    VkImportMemoryFdInfoKHR import{};
    import.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.pNext      = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd         = duplicate.get();

    VkMemoryAllocateInfo allocation{};
    allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext           = &import;
    allocation.allocationSize  = requirements.size;
    allocation.memoryTypeIndex = std::countr_zero(memory_types);
    const auto result = device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &image->external_memory);
    if (result != VK_SUCCESS) {
        check(result, "import DMA-BUF memory");
    }

    (void)duplicate.release();
    image->external_allocation_bytes = requirements.size;

    check(device->vk.vkBindImageMemory(device->device, image->image, image->external_memory, 0), "bind DMA-BUF image");

    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = image->image;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = format;
    view.subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel   = 0,
                             .levelCount     = 1,
                             .baseArrayLayer = 0,
                             .layerCount     = 1};

    check(device->vk.vkCreateImageView(device->device, &view, nullptr, &image->view), "create DMA-BUF sampled view");
    image->sampled_view = image->view;
    return image;
}

} // namespace miximus::gpu::detail
