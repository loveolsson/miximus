#include "gpu/detail/device.hpp"
#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/detail/dma_buf_image.hpp"
#include "gpu/detail/recording.hpp"
#include "gpu/detail/resource.hpp"
#include "logger/logger.hpp"

#include <bit>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace miximus::gpu::detail { namespace {

class dma_buf_image_test : public testing::Test
{
  public:
    std::shared_ptr<device_state_s>  device;
    std::shared_ptr<texture_state_s> exported;
    dma_buf_image_s                  descriptor;

    VkCommandPool producer_pool{};

    VkSemaphore producer_signal{};
    void        SetUp() override
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
        if (producer_pool != VK_NULL_HANDLE) {
            device->vk.vkQueueWaitIdle(device->submissions.queue);

            device->vk.vkDestroyCommandPool(device->device, producer_pool, nullptr);
        }

        if (producer_signal != VK_NULL_HANDLE) {
            device->vk.vkDestroySemaphore(device->device, producer_signal, nullptr);
        }

        if (descriptor.fd >= 0) {
            close(descriptor.fd);
        }

        exported.reset();
        if (device) {
            device->collect();
            EXPECT_EQ(device->errors.load(), 0U);
        }
    }

    bool export_image(VkFormat format, bool writable = false)
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
            query.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | (writable ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);

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
            info.extent      = {.width = 64, .height = 32, .depth = 1};
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
            descriptor.extent   = {.width = 64, .height = 32};
            descriptor.order    = format == VK_FORMAT_B8G8R8A8_UNORM ? channel_order_e::bgra : channel_order_e::rgba;
            descriptor.modifier = candidate.drmFormatModifier;
            descriptor.offset   = layout.offset;
            descriptor.stride   = layout.rowPitch;
            return true;
        }

        return false;
    }

    void start_producer()
    {
        VkCommandPoolCreateInfo pool{};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = device->queue_family;

        check(device->vk.vkCreateCommandPool(device->device, &pool, nullptr, &producer_pool),
              "create test producer pool");

        VkCommandBufferAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool        = producer_pool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;

        VkCommandBuffer commands{};

        check(device->vk.vkAllocateCommandBuffers(device->device, &allocate, &commands),
              "allocate test producer commands");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        check(device->vk.vkBeginCommandBuffer(commands, &begin), "begin test producer commands");

        VkImageMemoryBarrier2 barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = exported->image;
        barrier.subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel   = 0,
                                       .levelCount     = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount     = 1};

        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &barrier;

        device->vk.vkCmdPipelineBarrier2(commands, &dependency);

        VkClearColorValue color{};
        // Premultiplied SDR values exercise both sRGB transfer branches and
        // non-opaque alpha without introducing a host pixel-transfer path.
        color.float32[0] = 0.015F;
        color.float32[1] = 0.25F;
        color.float32[2] = 0.375F;
        color.float32[3] = 0.5F;

        device->vk.vkCmdClearColorImage(
            commands, exported->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask       = VK_ACCESS_2_NONE;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = device->queue_family;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;

        device->vk.vkCmdPipelineBarrier2(commands, &dependency);

        check(device->vk.vkEndCommandBuffer(commands), "end test producer commands");

        VkExportSemaphoreCreateInfo export_info{};
        export_info.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore.pNext = &export_info;

        check(device->vk.vkCreateSemaphore(device->device, &semaphore, nullptr, &producer_signal),
              "create test producer signal");

        VkSemaphoreSubmitInfo signal{};
        signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = producer_signal;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo command{};
        command.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = commands;

        VkSubmitInfo2 submit{};
        submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount   = 1;
        submit.pCommandBufferInfos      = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos    = &signal;

        check(device->vk.vkQueueSubmit2(device->submissions.queue, 1, &submit, VK_NULL_HANDLE), "submit test producer");

        VkSemaphoreGetFdInfoKHR get{};
        get.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        get.semaphore  = producer_signal;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd         = -1;

        check(device->vk.vkGetSemaphoreFdKHR(device->device, &get, &fd), "export test producer fence");
        // SYNC_FD may use -1 for an already-signalled payload. In that case
        // the producer has completed; there is no outstanding fence to attach.
        if (fd == -1) {
            return;
        }

        dma_buf_import_sync_file fence{};
        fence.flags      = DMA_BUF_SYNC_WRITE;
        fence.fd         = fd;
        const int result = ioctl(descriptor.fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &fence);
        const int error  = errno;
        if (fd >= 0) {
            close(fd);
        }

        if (result < 0) {
            throw std::system_error(error, std::generic_category(), "attach test producer fence");
        }
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

TEST_F(dma_buf_image_test, GpuCopyUsesPublishedProducerFenceAndRetires)
{
    using namespace std::chrono_literals;
    if (!export_image(VK_FORMAT_B8G8R8A8_UNORM, true)) {
        GTEST_SKIP() << "No exportable single-plane writable BGRA modifier";
    }

    // Separate logical device on the identical physical GPU models the external
    // producer, without modifying the application's submission worker.
    device_s consumer(device->options);
    auto     destination = consumer.create_texture(descriptor.extent);
    auto     context     = consumer.create_recording_context(1);
    start_producer();
    for (auto operation : {color_operation_e::none, color_operation_e::decode_srgb_premultiplied}) {
        auto recording = context.try_record();
        ASSERT_TRUE(recording);
        draw_s conversion;
        conversion.compositing = compositing_e::replace;
        conversion.transfer    = operation;
        auto completion        = dma_buf_copy_s::submit(*recording, descriptor, destination, conversion, 500ms);
        EXPECT_EQ(completion.wait(5s), wait_result_e::ready);
        EXPECT_TRUE(destination.idle());
    }

    EXPECT_NE(fcntl(descriptor.fd, F_GETFD), -1);
    EXPECT_TRUE(context.try_record());
    EXPECT_EQ(consumer.validation_errors(), 0U);
}

TEST_F(dma_buf_image_test, AbandonedCopyRetiresImportedResources)
{
    using namespace std::chrono_literals;
    if (!export_image(VK_FORMAT_R8G8B8A8_UNORM, true)) {
        GTEST_SKIP() << "No exportable single-plane writable RGBA modifier";
    }

    device_s consumer(device->options);
    auto     destination = consumer.create_texture(descriptor.extent);
    auto     context     = consumer.create_recording_context(1);
    start_producer();
    auto recording = context.try_record();
    ASSERT_TRUE(recording);
    draw_s invalid;
    invalid.clip = {0, 0, -1, 32};
    EXPECT_THROW(dma_buf_copy_s::submit(*recording, descriptor, destination, invalid, 500ms), std::invalid_argument);
    recording.reset();
    consumer.collect();
    recording = context.try_record();
    ASSERT_TRUE(recording);
    draw_s conversion;
    conversion.compositing = compositing_e::replace;
    auto completion        = dma_buf_copy_s::submit(*recording, descriptor, destination, conversion, 500ms);
    EXPECT_EQ(completion.wait(5s), wait_result_e::ready);
    EXPECT_NE(fcntl(descriptor.fd, F_GETFD), -1);
    EXPECT_EQ(consumer.validation_errors(), 0U);
}

}} // namespace miximus::gpu::detail
