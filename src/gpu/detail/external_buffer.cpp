#include "external_buffer.hpp"

#include "device.hpp"
#include "external_memory.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <cstring>

namespace miximus::gpu::detail {

external_buffer_s::external_buffer_s(device_s& device, const external_buffer_description_s& description)
{
    auto owner = device.state_;
    if (!owner->opaque_buffer_import || description.handle == native_handle_s::invalid || description.bytes < 80 ||
        description.allocation_bytes < description.bytes) {
        throw std::runtime_error("Opaque GPU buffer import is unavailable");
    }

    VkPhysicalDeviceIDProperties identity{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &identity};
    owner->instance_vk.vkGetPhysicalDeviceProperties2(owner->physical, &properties);
    if (std::memcmp(identity.deviceUUID, description.device_uuid.data(), VK_UUID_SIZE) != 0 ||
        std::memcmp(identity.driverUUID, description.driver_uuid.data(), VK_UUID_SIZE) != 0) {
        throw std::runtime_error("Decoded video and compositor use different GPU devices/drivers");
    }

    VkPhysicalDeviceExternalBufferInfo capability{.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                                  .usage      = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                  .handleType = opaque_memory_handle_type};
    VkExternalBufferProperties         supported{.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    owner->instance_vk.vkGetPhysicalDeviceExternalBufferProperties(owner->physical, &capability, &supported);
    if ((supported.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0U) {
        throw std::runtime_error("GPU component buffers cannot be imported on this device");
    }

    auto buffer             = std::make_shared<buffer_state_s>();
    buffer->owner           = owner;
    buffer->bytes           = description.bytes;
    buffer->access          = host_access_e::device_only;
    buffer->external_buffer = true;
    VkExternalMemoryBufferCreateInfo external{
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(opaque_memory_handle_type)};
    VkBufferCreateInfo create{.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext       = &external,
                              .size        = description.bytes,
                              .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(owner->vk.vkCreateBuffer(owner->device, &create, nullptr, &buffer->buffer), "create imported video buffer");
    VkMemoryRequirements requirements{};
    owner->vk.vkGetBufferMemoryRequirements(owner->device, buffer->buffer, &requirements);
    if (description.memory_type >= owner->memory.memoryTypeCount ||
        (requirements.memoryTypeBits & (1U << description.memory_type)) == 0U ||
        requirements.size > description.allocation_bytes) {
        throw std::runtime_error("Incompatible external video allocation");
    }

    buffer->external_memory = import_opaque_buffer_memory(owner->vk,
                                                          owner->device,
                                                          buffer->buffer,
                                                          description.allocation_bytes,
                                                          description.memory_type,
                                                          description.handle);
    check(owner->vk.vkBindBufferMemory(owner->device, buffer->buffer, buffer->external_memory, 0), "bind video import");
    buffer_ = buffer_s(std::move(buffer));
}

completion_s external_buffer_s::convert(recording_s& record, const texture_s& target, const color_transform_s& color)
{
    auto& state = *record.state_;
    state.retain(buffer_.state_);
    VkBufferMemoryBarrier2 barrier{.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                   .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                                   .dstQueueFamilyIndex = state.owner->queue_family,
                                   .buffer              = buffer_.state_->buffer,
                                   .size                = VK_WHOLE_SIZE};
    VkDependencyInfo       dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
    state.owner->vk.vkCmdPipelineBarrier2(state.arena->commands, &dependency);
    record.unpack_video(buffer_, target, color);
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask       = VK_ACCESS_2_NONE;
    barrier.srcQueueFamilyIndex = state.owner->queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    state.owner->vk.vkCmdPipelineBarrier2(state.arena->commands, &dependency);
    record.generate_mip_maps(target);
    return record.submit();
}
} // namespace miximus::gpu::detail
