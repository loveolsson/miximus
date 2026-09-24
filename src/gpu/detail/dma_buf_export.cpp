#include "dma_buf_export.hpp"

#include "device.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <atomic>
#include <bit>
#include <stdexcept>
#include <unistd.h>

namespace miximus::gpu::detail {

struct dma_buf_export_s::state_s
{
    std::shared_ptr<texture_state_s> exported;
    dma_buf_image_s                  descriptor;
    std::atomic_bool                 foreign{};

    ~state_s()
    {
        if (descriptor.fd >= 0) {
            close(descriptor.fd);
        }
    }

    void initialize(const std::shared_ptr<device_state_s>& device, extent_s extent)
    {
        if (!device->external_image_import.enabled) {
            throw std::runtime_error("DMA-BUF sharing extensions are not enabled");
        }
        if (!extent.width || !extent.height || extent.width > device->properties.limits.maxImageDimension2D ||
            extent.height > device->properties.limits.maxImageDimension2D) {
            throw std::invalid_argument("Invalid DMA-BUF export dimensions");
        }
        constexpr auto                       format = VK_FORMAT_R8G8B8A8_UNORM;
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
        for (const auto& candidate : entries) {
            constexpr auto features =
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if (candidate.drmFormatModifierPlaneCount != 1 ||
                (candidate.drmFormatModifierTilingFeatures & features) != features) {
                continue;
            }
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query{};
            modifier_query.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
            modifier_query.drmFormatModifier = candidate.drmFormatModifier;
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
            query.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            VkExternalImageFormatProperties external_properties{};
            external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            properties.pNext = &external_properties;
            const auto result =
                device->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device->physical, &query, &properties);
            constexpr auto required =
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
            if (result != VK_SUCCESS || extent.width > properties.imageFormatProperties.maxExtent.width ||
                extent.height > properties.imageFormatProperties.maxExtent.height ||
                (external_properties.externalMemoryProperties.externalMemoryFeatures & required) != required) {
                continue;
            }
            exported           = std::make_shared<texture_state_s>();
            exported->owner    = device;
            exported->extent   = extent;
            exported->format   = format_e::rgba_unorm8;
            exported->sampling = sampling_e::linear;
            exported->layouts  = {VK_IMAGE_LAYOUT_UNDEFINED};
            VkImageDrmFormatModifierListCreateInfoEXT modifier{};
            modifier.sType                  = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
            modifier.drmFormatModifierCount = 1;
            modifier.pDrmFormatModifiers    = &candidate.drmFormatModifier;
            VkExternalMemoryImageCreateInfo external{};
            external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            external.pNext       = &modifier;
            external.handleTypes = external_query.handleType;
            VkImageCreateInfo info{};
            info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.pNext       = &external;
            info.imageType   = query.type;
            info.format      = format;
            info.extent      = {.width = extent.width, .height = extent.height, .depth = 1};
            info.mipLevels   = 1;
            info.arrayLayers = 1;
            info.samples     = VK_SAMPLE_COUNT_1_BIT;
            info.tiling      = query.tiling;
            info.usage       = query.usage;
            check(device->vk.vkCreateImage(device->device, &info, nullptr, &exported->image),
                  "create DMA-BUF export image");
            VkMemoryRequirements requirements{};
            device->vk.vkGetImageMemoryRequirements(device->device, exported->image, &requirements);
            VkMemoryDedicatedAllocateInfo dedicated{};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated.image = exported->image;
            VkExportMemoryAllocateInfo export_memory{};
            export_memory.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            export_memory.pNext       = &dedicated;
            export_memory.handleTypes = external_query.handleType;
            VkMemoryAllocateInfo allocation{};
            allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.pNext           = &export_memory;
            allocation.allocationSize  = requirements.size;
            allocation.memoryTypeIndex = std::countr_zero(requirements.memoryTypeBits);
            // Driver memory-type order does not express a performance preference.
            // NVIDIA exposes compatible system memory before device-local VRAM.
            for (uint32_t index = 0; index < device->memory.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (uint32_t{1} << index)) != 0U &&
                    (device->memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                    allocation.memoryTypeIndex = index;
                    break;
                }
            }
            check(device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &exported->external_memory),
                  "allocate DMA-BUF export image");
            check(device->vk.vkBindImageMemory(device->device, exported->image, exported->external_memory, 0),
                  "bind DMA-BUF export image");
            VkMemoryGetFdInfoKHR get_fd{};
            get_fd.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            get_fd.memory     = exported->external_memory;
            get_fd.handleType = external_query.handleType;
            check(device->vk.vkGetMemoryFdKHR(device->device, &get_fd, &descriptor.fd), "export DMA-BUF export image");
            VkImageSubresource subresource{};
            subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
            VkSubresourceLayout layout{};
            device->vk.vkGetImageSubresourceLayout(device->device, exported->image, &subresource, &layout);
            descriptor.extent                   = extent;
            descriptor.order                    = channel_order_e::rgba;
            descriptor.modifier                 = candidate.drmFormatModifier;
            descriptor.offset                   = layout.offset;
            descriptor.stride                   = layout.rowPitch;
            exported->external_allocation_bytes = requirements.size;
            VkImageViewCreateInfo view{};
            view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image            = exported->image;
            view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view.format           = format;
            view.subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .baseMipLevel   = 0,
                                     .levelCount     = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount     = 1};
            check(device->vk.vkCreateImageView(device->device, &view, nullptr, &exported->view),
                  "create DMA-BUF export view");
            exported->sampled_view = exported->view;
            return;
        }
        throw std::runtime_error("No exportable single-plane renderable RGBA DMA-BUF modifier");
    }
};

dma_buf_export_s::dma_buf_export_s(device_s& device, extent_s extent)
    : state_(std::make_shared<state_s>())
{
    state_->initialize(device.state_, extent);
}
dma_buf_export_s::~dma_buf_export_s() = default;

dma_buf_image_s dma_buf_export_s::descriptor() const { return state_->descriptor; }
size_t          dma_buf_export_s::allocation_bytes() const { return state_->exported->external_allocation_bytes; }

void dma_buf_export_s::copy(recording_s& record, const texture_s& source, const draw_s& conversion)
{
    if (!record.state_ || record.state_->submission_attempted) {
        throw std::invalid_argument("DMA-BUF export requires an active recording");
    }
    auto&      recording = *record.state_;
    const auto image     = state_->exported;
    if (recording.owner != image->owner || recording.layouts.contains(image.get())) {
        throw std::invalid_argument("DMA-BUF export requires one write on its owning device");
    }
    recording.retain(image);
    // Seed the local layout: the ordinary prologue must not run ahead of this
    // explicit foreign-ownership acquire. First use starts uninitialized.
    recording.layouts.emplace(image.get(), std::vector{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    const bool            foreign = state_->foreign.load();
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout           = foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = foreign ? recording.owner->queue_family : VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image->image;
    barrier.subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .baseMipLevel   = 0,
                                   .levelCount     = 1,
                                   .baseArrayLayer = 0,
                                   .layerCount     = 1};
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    recording.owner->vk.vkCmdPipelineBarrier2(recording.arena->commands, &dependency);
    record.draw(source, texture_s(image), conversion);
    recording.transition(
        image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask       = VK_ACCESS_2_NONE;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = recording.owner->queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    recording.owner->vk.vkCmdPipelineBarrier2(recording.arena->commands, &dependency);
    record.on_submitted([state = state_](completion_s /* completion */) { state->foreign.store(true); });
}

} // namespace miximus::gpu::detail
