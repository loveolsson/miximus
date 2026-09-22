#include "gpu/detail/device.hpp"
#include "gpu/detail/dma_buf_image.hpp"
#include "logger/logger.hpp"

#include <bit>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

namespace miximus::gpu::detail { namespace {

class dma_buf_image_test : public testing::Test
{
  protected:
    std::shared_ptr<device_state_s>  device;
    std::shared_ptr<texture_state_s> exported;
    dma_buf_image_s                  descriptor;

    void SetUp() override
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::warn);
        }
        device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const auto* uuid = std::getenv("MIXIMUS_VULKAN_DEVICE")) {
            options.device_uuid = uuid;
        }
        options.external_image_import = true;
        device                        = std::make_shared<device_state_s>();
        device->initialize(options);
        if (!device->external_image_import.enabled) {
            GTEST_SKIP() << "Selected device lacks DMA-BUF import extensions";
        }
    }

    void TearDown() override
    {
        if (descriptor.fd >= 0) {
            close(descriptor.fd);
        }
        exported.reset();
        if (device) {
            device->collect();
            EXPECT_EQ(device->errors.load(), 0U);
        }
    }

    bool export_image(VkFormat format)
    {
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
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
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
            query.usage  = VK_IMAGE_USAGE_SAMPLED_BIT;
            VkExternalImageFormatProperties external_properties{};
            external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            properties.pNext = &external_properties;
            const auto result =
                device->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device->physical, &query, &properties);
            constexpr auto required =
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
            if (result != VK_SUCCESS ||
                (external_properties.externalMemoryProperties.externalMemoryFeatures & required) != required) {
                continue;
            }
            exported        = std::make_shared<texture_state_s>();
            exported->owner = device;
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
            info.extent      = {64, 32, 1};
            info.mipLevels   = 1;
            info.arrayLayers = 1;
            info.samples     = VK_SAMPLE_COUNT_1_BIT;
            info.tiling      = query.tiling;
            info.usage       = query.usage;
            check(device->vk.vkCreateImage(device->device, &info, nullptr, &exported->image),
                  "create DMA-BUF test image");
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
            check(device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &exported->external_memory),
                  "allocate DMA-BUF test image");
            check(device->vk.vkBindImageMemory(device->device, exported->image, exported->external_memory, 0),
                  "bind DMA-BUF test image");
            VkMemoryGetFdInfoKHR get_fd{};
            get_fd.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            get_fd.memory     = exported->external_memory;
            get_fd.handleType = external_query.handleType;
            check(device->vk.vkGetMemoryFdKHR(device->device, &get_fd, &descriptor.fd), "export DMA-BUF test image");
            VkImageSubresource subresource{};
            subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
            VkSubresourceLayout layout{};
            device->vk.vkGetImageSubresourceLayout(device->device, exported->image, &subresource, &layout);
            descriptor.extent   = {64, 32};
            descriptor.order    = format == VK_FORMAT_B8G8R8A8_UNORM ? channel_order_e::bgra : channel_order_e::rgba;
            descriptor.modifier = candidate.drmFormatModifier;
            descriptor.offset   = layout.offset;
            descriptor.stride   = layout.rowPitch;
            return true;
        }
        return false;
    }
};

TEST_F(dma_buf_image_test, ImportsRgbaWithoutConsumingBorrowedFd)
{
    if (!export_image(VK_FORMAT_R8G8B8A8_UNORM)) {
        GTEST_SKIP() << "No exportable single-plane RGBA modifier";
    }
    auto first  = import_dma_buf_image(device, descriptor);
    auto second = import_dma_buf_image(device, descriptor);
    EXPECT_NE(first->image, second->image);
    EXPECT_EQ(first->extent, descriptor.extent);
    first.reset();
    second.reset();
    device->collect();
    EXPECT_NE(fcntl(descriptor.fd, F_GETFD), -1);
}

TEST_F(dma_buf_image_test, ImportsBgraAndRetainsMemoryAfterExporterRelease)
{
    if (!export_image(VK_FORMAT_B8G8R8A8_UNORM)) {
        GTEST_SKIP() << "No exportable single-plane BGRA modifier";
    }
    auto imported = import_dma_buf_image(device, descriptor);
    close(descriptor.fd);
    descriptor.fd = -1;
    exported.reset();
    device->collect();
    EXPECT_NE(imported->sampled_view, VK_NULL_HANDLE);
    EXPECT_GT(imported->external_allocation_bytes, 0U);
}

TEST_F(dma_buf_image_test, RejectsUnsupportedLayoutWithoutClosingBorrowedFd)
{
    if (!export_image(VK_FORMAT_R8G8B8A8_UNORM)) {
        GTEST_SKIP() << "No exportable single-plane RGBA modifier";
    }
    auto invalid     = descriptor;
    invalid.modifier = UINT64_MAX;
    EXPECT_THROW(import_dma_buf_image(device, invalid), std::runtime_error);
    invalid        = descriptor;
    invalid.stride = 1;
    EXPECT_THROW(import_dma_buf_image(device, invalid), std::invalid_argument);
    invalid       = descriptor;
    invalid.order = channel_order_e::argb;
    EXPECT_THROW(import_dma_buf_image(device, invalid), std::invalid_argument);
    EXPECT_NE(fcntl(descriptor.fd, F_GETFD), -1);
    EXPECT_NO_THROW(import_dma_buf_image(device, descriptor));
}

}} // namespace miximus::gpu::detail
