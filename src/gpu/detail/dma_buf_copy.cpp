#include "dma_buf_copy.hpp"

#include "device.hpp"

#include <cerrno>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace miximus::gpu::detail {
namespace {

struct read_fence_s : resource_state_s
{
    VkSemaphore semaphore{};

    ~read_fence_s()
    {
        if (semaphore) {
            owner->retire(last_use_timeline_value.load(),
                          [device = owner->device, destroy = owner->vk.vkDestroySemaphore, handle = semaphore] {
                              destroy(device, handle, nullptr);
                          });
        }
    }
};

std::shared_ptr<read_fence_s> import_read_fence(const std::shared_ptr<device_state_s>& device, int dma_buf)
{
    VkPhysicalDeviceExternalSemaphoreInfo query{};
    query.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties properties{};
    properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    device->instance_vk.vkGetPhysicalDeviceExternalSemaphoreProperties(device->physical, &query, &properties);
    if ((properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
        throw std::runtime_error("DMA-BUF read requires importable sync-file semaphores");
    }
    auto fence   = std::make_shared<read_fence_s>();
    fence->owner = device;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    check(device->vk.vkCreateSemaphore(device->device, &create, nullptr, &fence->semaphore),
          "create DMA-BUF read semaphore");

    dma_buf_export_sync_file export_fence{};
    export_fence.flags = DMA_BUF_SYNC_READ;
    export_fence.fd    = -1;
    // No DMA_BUF_IOCTL_SYNC or CPU mapping: export the producer's write fences.
    // No fallback to an unsynchronized read if this contract is unavailable.
    if (ioctl(dma_buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_fence) < 0) {
        throw std::system_error(errno, std::generic_category(), "export DMA-BUF write fence");
    }
    VkImportSemaphoreFdInfoKHR import{};
    import.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import.semaphore  = fence->semaphore;
    import.flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import.fd         = export_fence.fd;
    const auto result = device->vk.vkImportSemaphoreFdKHR(device->device, &import);
    if (result != VK_SUCCESS) {
        close(export_fence.fd);
        check(result, "import DMA-BUF write fence");
    }
    return fence;
}

} // namespace

completion_s dma_buf_copy_s::submit(recording_s&           record,
                                    const dma_buf_image_s& source,
                                    const texture_s&       destination,
                                    const draw_s&          conversion)
{
    if (!record.state_ || record.state_->submission_attempted) {
        throw std::invalid_argument("DMA-BUF copy requires an active recording");
    }
    auto& state = *record.state_;
    if (!destination.state_ || destination.state_->owner != state.owner) {
        throw std::invalid_argument("DMA-BUF destination must belong to the recording device");
    }
    auto image = import_dma_buf_image(state.owner, source);
    auto fence = import_read_fence(state.owner, source.fd);
    state.retain(fence);
    state.retain(image);
    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = fence->semaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    // As in the CUDA bridge, prevent the submission prologue from transitioning
    // the imported image before the explicit ownership-acquire barrier.
    state.layouts.emplace(image.get(), std::vector{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = state.owner->queue_family;
    barrier.image               = image->image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    state.owner->vk.vkCmdPipelineBarrier2(state.arena->commands, &dependency);

    record.draw(texture_s(image), destination, conversion);
    state.transition(image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask       = VK_ACCESS_2_NONE;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = state.owner->queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    state.owner->vk.vkCmdPipelineBarrier2(state.arena->commands, &dependency);
    return enqueue_recording(record.state_, std::span{&wait, 1});
}

} // namespace miximus::gpu::detail
