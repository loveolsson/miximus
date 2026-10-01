#include "device.hpp"
#include "external_image_export_state.hpp"
#include "external_image_sync.hpp"
#include "recording.hpp"

namespace miximus::gpu::detail {

external_image_export_s::external_image_export_s(device_s& device, extent_s extent)
    : state_(std::make_shared<state_s>())
{
    state_->initialize(device.state_, extent);
}

external_image_export_s::~external_image_export_s() = default;

external_image_s external_image_export_s::descriptor() const { return state_->descriptor; }
size_t external_image_export_s::allocation_bytes() const { return state_->exported->external_allocation_bytes; }

void external_image_export_s::copy(recording_s& record, const texture_s& source, const draw_s& conversion)
{
    if (!record.state_ || record.state_->submission_attempted) {
        throw std::invalid_argument("External image export requires an active recording");
    }

    auto&      recording = *record.state_;
    const auto image     = state_->exported;
    if (recording.owner != image->owner || recording.layouts.contains(image.get())) {
        throw std::invalid_argument("External image export requires one write on its owning device");
    }

    recording.retain(image);
    // Seed the local layout: the ordinary prologue must not run ahead of this
    // explicit foreign-ownership acquire. First use starts uninitialized.
    recording.layouts.emplace(image.get(), std::vector{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    const bool foreign = state_->foreign.load();

    VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = foreign ? external_image_queue_family : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = foreign ? recording.owner->queue_family : VK_QUEUE_FAMILY_IGNORED,
        .image               = image->image,
        .subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel   = 0,
                                .levelCount     = 1,
                                .baseArrayLayer = 0,
                                .layerCount     = 1},
    };

    VkDependencyInfo dependency{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };

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
    barrier.dstQueueFamilyIndex = external_image_queue_family;

    recording.owner->vk.vkCmdPipelineBarrier2(recording.arena->commands, &dependency);
    record.on_submitted([state = state_](const completion_s& /* completion */) { state->foreign.store(true); });
}

} // namespace miximus::gpu::detail
