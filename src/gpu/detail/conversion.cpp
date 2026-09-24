#include "device.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <stdexcept>

namespace miximus::gpu::detail {

void recording_state_s::convert(const std::shared_ptr<buffer_state_s>&  buffer,
                                const std::shared_ptr<texture_state_s>& image,
                                size_t                                  stride,
                                conversion_operation_e                  operation,
                                const color_transform_s&                color,
                                channel_order_e                         order)
{
    const bool pack = operation == conversion_operation_e::pack_v210;
    if (!owner->buffer_conversion) {
        throw std::runtime_error("buffer conversion unsupported on this device");
    }

    if (image->format != format_e::rgba_unorm16) {
        throw std::invalid_argument("RGBA buffer conversion requires UNORM16 working storage");
    }

    const size_t minimum_stride = ((size_t{image->extent.width} + 5) / 6) * 16;
    if (stride == 0U) {
        stride = minimum_stride;
    }

    if (((stride % 4) != 0U) || stride < minimum_stride || stride / 4 > UINT32_MAX ||
        image->extent.height > buffer->bytes / stride ||
        buffer->bytes > owner->properties.limits.maxStorageBufferRange) {
        throw std::invalid_argument("invalid conversion buffer/stride");
    }

    const uint32_t dispatch_width = pack ? static_cast<uint32_t>((stride / 4 + 3) / 4) : image->extent.width;
    const uint32_t groups_x       = (dispatch_width + 15) / 16;
    const uint32_t groups_y       = (image->extent.height + 15) / 16;
    if (groups_x > owner->properties.limits.maxComputeWorkGroupCount[0] ||
        groups_y > owner->properties.limits.maxComputeWorkGroupCount[1]) {
        throw std::invalid_argument("conversion dispatch exceeds device limits");
    }

    retain(buffer);
    buffer_barrier(buffer,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   pack ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    transition(image,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               pack ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const auto descriptor = allocate_descriptor(owner->drawing->conversion_layout);

    VkDescriptorBufferInfo              storage{buffer->buffer, 0, buffer->bytes};
    VkDescriptorImageInfo               image_info{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (auto& write : writes) {
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = descriptor;
        write.descriptorCount = 1;
    }

    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo    = &storage;
    writes[1].dstBinding     = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo     = &image_info;
    owner->vk.vkUpdateDescriptorSets(owner->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    // Compute shaders address packed storage in 32-bit words, including SDK row padding.
    struct conversion_parameters_s
    {
        uint32_t          image_width{};
        uint32_t          image_height{};
        uint32_t          row_stride_words{};
        channel_order_e   channel_order{};
        color_transform_s color;
    };

    static_assert(sizeof(conversion_parameters_s) == 128);
    static_assert(offsetof(conversion_parameters_s, row_stride_words) == 8);
    static_assert(offsetof(conversion_parameters_s, channel_order) == 12);
    static_assert(offsetof(conversion_parameters_s, color) == 16);
    static_assert(offsetof(color_transform_s, gamut) == 48);
    static_assert(offsetof(color_transform_s, offset) == 96);

    const conversion_parameters_s parameters{
        .image_width      = image->extent.width,
        .image_height     = image->extent.height,
        .row_stride_words = static_cast<uint32_t>(stride / 4),
        .channel_order    = order,
        .color            = color,
    };

    owner->vk.vkCmdBindPipeline(
        arena->commands, VK_PIPELINE_BIND_POINT_COMPUTE, owner->drawing->conversion_pipelines[operation]);
    owner->vk.vkCmdBindDescriptorSets(arena->commands,
                                      VK_PIPELINE_BIND_POINT_COMPUTE,
                                      owner->drawing->conversion_pipeline_layout,
                                      0,
                                      1,
                                      &descriptor,
                                      0,
                                      nullptr);
    owner->vk.vkCmdPushConstants(arena->commands,
                                 owner->drawing->conversion_pipeline_layout,
                                 VK_SHADER_STAGE_COMPUTE_BIT,
                                 0,
                                 sizeof(parameters),
                                 &parameters);
    owner->vk.vkCmdDispatch(arena->commands, groups_x, groups_y, 1);
    if (pack && (buffer->mapped != nullptr)) {
        buffer_barrier(buffer, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
}
} // namespace miximus::gpu::detail

namespace miximus::gpu {

void recording_s::unpack_v210(const buffer_s&          source,
                              const texture_s&         destination,
                              const color_transform_s& color,
                              size_t                   row_stride)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid conversion handles");
    }

    state_->convert(source.state_, destination.state_, row_stride, detail::conversion_operation_e::unpack_v210, color);
}

void recording_s::pack_v210(const texture_s&         source,
                            const buffer_s&          destination,
                            const color_transform_s& color,
                            size_t                   row_stride)
{
    if (!state_ || !source || !destination) {
        throw std::invalid_argument("invalid conversion handles");
    }

    state_->convert(destination.state_, source.state_, row_stride, detail::conversion_operation_e::pack_v210, color);
}
} // namespace miximus::gpu
