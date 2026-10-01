#include "device.hpp"
#include "external_image_export_state.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <drm_fourcc.h>
#include <span>
#include <stdexcept>

namespace miximus::gpu::detail {
namespace {
constexpr VkFormat          EXPORT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags EXPORT_USAGE  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

uint64_t choose_export_modifier(const std::shared_ptr<device_state_s>& device, extent_s extent)
{
    VkDrmFormatModifierPropertiesListEXT modifiers{
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };

    VkFormatProperties2 formats{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifiers,
    };

    device->instance_vk.vkGetPhysicalDeviceFormatProperties2(device->physical, EXPORT_FORMAT, &formats);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = entries.data();

    device->instance_vk.vkGetPhysicalDeviceFormatProperties2(device->physical, EXPORT_FORMAT, &formats);
    entries.resize(modifiers.drmFormatModifierCount);
    for (const auto& candidate : entries) {
        // NVIDIA advertises large block heights first. Vulkan can allocate
        // them for tiny images, but EGL rejects those exports. Choose a
        // block height that fits the image, with one GOB as the minimum.
        // Keep the driver's ordering for larger images and other vendors.
        const auto layout_modifier = candidate.drmFormatModifier;
        if (fourcc_mod_is_vendor(layout_modifier, NVIDIA) && (layout_modifier & 0x10U) != 0U) {
            const auto generation   = (layout_modifier >> 20U) & 3U;
            const auto gob_height   = generation == 1 ? 4U : 8U;
            const auto block_height = gob_height << (layout_modifier & 0xfU);
            if (block_height > std::max(extent.height, gob_height)) {
                continue;
            }
        }

        constexpr auto features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (candidate.drmFormatModifierPlaneCount != 1 ||
            (candidate.drmFormatModifierTilingFeatures & features) != features) {
            continue;
        }

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query{
            .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = candidate.drmFormatModifier,
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
            .format = EXPORT_FORMAT,
            .type   = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage  = EXPORT_USAGE,
        };

        VkExternalImageFormatProperties external_properties{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };

        VkImageFormatProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &external_properties,
        };
        const auto result =
            device->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device->physical, &query, &properties);
        constexpr auto required = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        if (result != VK_SUCCESS || extent.width > properties.imageFormatProperties.maxExtent.width ||
            extent.height > properties.imageFormatProperties.maxExtent.height ||
            (external_properties.externalMemoryProperties.externalMemoryFeatures & required) != required) {
            continue;
        }

        return candidate.drmFormatModifier;
    }

    throw std::runtime_error("No exportable single-plane renderable RGBA DMA-BUF modifier");
}
} // namespace

void external_image_export_s::state_s::initialize(const std::shared_ptr<device_state_s>& device, extent_s extent)
{
    if (device->external_image_import != external_image_import_support_e::supported) {
        throw std::runtime_error("DMA-BUF sharing extensions are not enabled");
    }

    if (extent.width == 0 || extent.height == 0 || extent.width > device->properties.limits.maxImageDimension2D ||
        extent.height > device->properties.limits.maxImageDimension2D) {
        throw std::invalid_argument("Invalid DMA-BUF export dimensions");
    }

    const auto selected_modifier = choose_export_modifier(device, extent);

    exported           = std::make_shared<texture_state_s>();
    exported->owner    = device;
    exported->extent   = extent;
    exported->format   = format_e::rgba_unorm8;
    exported->sampling = sampling_e::linear;
    exported->layouts  = {VK_IMAGE_LAYOUT_UNDEFINED};

    VkImageDrmFormatModifierListCreateInfoEXT modifier{
        .sType                  = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers    = &selected_modifier,
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
        .format      = EXPORT_FORMAT,
        .extent      = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage       = EXPORT_USAGE,
    };

    check(device->vk.vkCreateImage(device->device, &info, nullptr, &exported->image), "create DMA-BUF export image");

    VkMemoryRequirements requirements{};

    device->vk.vkGetImageMemoryRequirements(device->device, exported->image, &requirements);

    VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = exported->image,
    };

    VkExportMemoryAllocateInfo export_memory{
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext       = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    auto memory_type = static_cast<uint32_t>(std::countr_zero(requirements.memoryTypeBits));
    // Driver memory-type order does not express a performance preference.
    // NVIDIA exposes compatible system memory before device-local VRAM.
    for (uint32_t index = 0; index < device->memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (uint32_t{1} << index)) != 0U &&
            (std::span{device->memory.memoryTypes}[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memory_type = index;
            break;
        }
    }

    VkMemoryAllocateInfo allocation{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &export_memory,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = memory_type,
    };

    check(device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &exported->external_memory),
          "allocate DMA-BUF export image");

    check(device->vk.vkBindImageMemory(device->device, exported->image, exported->external_memory, 0),
          "bind DMA-BUF export image");

    VkMemoryGetFdInfoKHR get_fd{
        .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory     = exported->external_memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    int fd{-1};
    check(device->vk.vkGetMemoryFdKHR(device->device, &get_fd, &fd), "export DMA-BUF export image");
    exported->external_memory_handle.reset(fd);
    descriptor.handle = exported->external_memory_handle.get();

    VkImageSubresource subresource{
        .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
    };

    VkSubresourceLayout layout{};

    device->vk.vkGetImageSubresourceLayout(device->device, exported->image, &subresource, &layout);
    descriptor.extent                   = extent;
    descriptor.order                    = channel_order_e::rgba;
    descriptor.modifier                 = selected_modifier;
    descriptor.offset                   = layout.offset;
    descriptor.stride                   = layout.rowPitch;
    exported->external_allocation_bytes = requirements.size;

    VkImageViewCreateInfo view{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = exported->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = EXPORT_FORMAT,
        .subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel   = 0,
                             .levelCount     = 1,
                             .baseArrayLayer = 0,
                             .layerCount     = 1},
    };

    check(device->vk.vkCreateImageView(device->device, &view, nullptr, &exported->view), "create DMA-BUF export view");
    exported->sampled_view = exported->view;
}

} // namespace miximus::gpu::detail
