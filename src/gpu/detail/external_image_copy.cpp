#include "external_image_copy.hpp"

#include "device.hpp"
#include "external_image_sync.hpp"
#include "recording.hpp"
#include "resource.hpp"

namespace miximus::gpu::detail {

completion_s external_image_copy_s::submit(recording_s&              record,
                                           const external_image_s&   source,
                                           const texture_s&          destination,
                                           const draw_s&             conversion,
                                           std::chrono::milliseconds readiness_timeout)
{
    if (readiness_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Negative external image readiness timeout");
    }

    if (!record.state_ || record.state_->submission_attempted) {
        throw std::invalid_argument("External image copy requires an active recording");
    }

    auto& state = *record.state_;
    if (!destination.state_ || destination.state_->owner != state.owner) {
        throw std::invalid_argument("External image destination must belong to the recording device");
    }

    auto image = import_external_image(state.owner, source);
    auto fence = prepare_external_image_read(state.owner, source, readiness_timeout);
    if (fence.lifetime) {
        state.retain(fence.lifetime);
    }
    state.retain(image);

    VkSemaphoreSubmitInfo wait{
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = fence.semaphore,
        .value       = 0,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };

    // As in the CUDA bridge, prevent the submission prologue from transitioning
    // the imported image before the explicit ownership-acquire barrier.
    state.layouts.emplace(image.get(), std::vector{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

    VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = 0,
        .srcAccessMask       = 0,
        .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = external_image_queue_family,
        .dstQueueFamilyIndex = state.owner->queue_family,
        .image               = image->image,
        .subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel   = 0,
                                .levelCount     = 1,
                                .baseArrayLayer = 0,
                                .layerCount     = 1},
    };

    VkDependencyInfo dependency{
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 0,
        .pMemoryBarriers          = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers    = nullptr,
        .imageMemoryBarrierCount  = 1,
        .pImageMemoryBarriers     = &barrier,
    };

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
    barrier.dstQueueFamilyIndex = external_image_queue_family;

    state.owner->vk.vkCmdPipelineBarrier2(state.arena->commands, &dependency);
    return enqueue_recording(record.state_,
                             std::span{&wait, fence.semaphore != VK_NULL_HANDLE ? size_t{1} : size_t{0}});
}

} // namespace miximus::gpu::detail
