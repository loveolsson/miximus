#include "device.hpp"
#include "external_video_copy.hpp"
#include "recording.hpp"
#include "resource.hpp"

namespace miximus::gpu::detail {

Microsoft::WRL::ComPtr<ID3D11Device> external_video_copy_s::create_producer_device(device_s& device)
{
    return create_d3d11_producer_device(device.state_);
}

external_video_copy_s::external_video_copy_s(device_s& device, const d3d11_image_description_s& image, size_t bytes)
    : image_(import_d3d11_image(device.state_, image))
    , components_(device.create_buffer(bytes, host_access_e::device_only))
{
    if (image.usage != VK_IMAGE_USAGE_TRANSFER_SRC_BIT || image.dxgi_format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        bytes < sizeof(video_buffer_header_s) + (size_t{image.extent.width} * image.extent.height * 4)) {
        throw std::invalid_argument("Invalid shared video byte carrier");
    }
}

completion_s external_video_copy_s::convert(recording_s&                 record,
                                            const texture_s&             target,
                                            const video_buffer_header_s& header,
                                            const color_transform_s&     color)
{
    auto& state = *record.state_;
    state.retain(image_);
    state.retain(components_.state_);
    const auto& vk      = state.owner->vk;
    const auto  command = state.arena->commands;

    VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = state.owner->queue_family,
        .image               = image_->image,
        .subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vk.vkCmdPipelineBarrier2(command, &dependency);
    state.buffer_barrier(components_.state_, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vk.vkCmdUpdateBuffer(command, components_.state_->buffer, 0, sizeof(header), &header);

    const VkBufferImageCopy region{
        .bufferOffset     = sizeof(header),
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
        .imageExtent      = {image_->extent.width, image_->extent.height, 1},
    };
    vk.vkCmdCopyImageToBuffer(
        command, image_->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, components_.state_->buffer, 1, &region);

    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask       = VK_ACCESS_2_NONE;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = state.owner->queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vk.vkCmdPipelineBarrier2(command, &dependency);

    record.unpack_video(components_, target, color);
    record.generate_mip_maps(target);
    return record.submit();
}

} // namespace miximus::gpu::detail
