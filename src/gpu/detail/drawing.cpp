#include "device.hpp"
#include "recording.hpp"
#include "resource.hpp"

#include <algorithm>
#include <stdexcept>

namespace miximus::gpu::detail {

void recording_state_s::draw(const std::shared_ptr<texture_state_s>&      a,
                             const std::shared_ptr<texture_state_s>&      b,
                             const std::shared_ptr<texture_state_s>&      target,
                             std::span<const std::byte>                   parameters,
                             compositing_e                                compositing,
                             draw_operation_e                             operation,
                             std::array<bool, 2>                          minifying,
                             const std::optional<std::array<int32_t, 4>>& clip)
{
    if (a == target || b == target || a->format == format_e::r32_uint || b->format == format_e::r32_uint ||
        target->format == format_e::r32_uint) {
        throw std::invalid_argument("invalid sampled draw");
    }

    VkRect2D scissor{
        {0,                    0                    },
        {target->extent.width, target->extent.height}
    };

    if (clip) {
        const auto& rect = *clip;
        if (rect[2] < 0 || rect[3] < 0) {
            throw std::invalid_argument("negative clipping extent");
        }

        const auto left   = std::clamp<int64_t>(rect[0], 0, target->extent.width);
        const auto top    = std::clamp<int64_t>(rect[1], 0, target->extent.height);
        const auto right  = std::clamp<int64_t>(int64_t{rect[0]} + rect[2], left, target->extent.width);
        const auto bottom = std::clamp<int64_t>(int64_t{rect[1]} + rect[3], top, target->extent.height);
        if (left == right || top == bottom) {
            return;
        }

        scissor = {
            .offset = {static_cast<int32_t>(left),          static_cast<int32_t>(top)          },
            .extent = {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}
        };
    }

    prepare_sampled(a, minifying[0] || (a == b && minifying[1]));
    if (a != b) {
        prepare_sampled(b, minifying[1]);
    }

    transition(target,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const auto descriptor = allocate_descriptor(owner->drawing->texture_layout);

    const std::array<VkDescriptorImageInfo, 2> sampled{
        {{.sampler     = a->sampling == sampling_e::nearest ? owner->drawing->nearest_sampler : owner->drawing->sampler,
          .imageView   = minifying[0] ? a->sampled_view : a->view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {.sampler     = b->sampling == sampling_e::nearest ? owner->drawing->nearest_sampler : owner->drawing->sampler,
          .imageView   = minifying[1] ? b->sampled_view : b->view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
    };

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes.at(i).sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes.at(i).dstSet          = descriptor;
        writes.at(i).dstBinding      = i;
        writes.at(i).descriptorCount = 1;
        writes.at(i).descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.at(i).pImageInfo      = &sampled.at(i);
    }

    owner->vk.vkUpdateDescriptorSets(owner->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    VkRenderingAttachmentInfo attachment{};
    attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView   = target->view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo render{};
    render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea.extent    = {.width = target->extent.width, .height = target->extent.height};
    render.layerCount           = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments    = &attachment;
    const auto command_buffer   = arena->commands;
    owner->vk.vkCmdBeginRendering(command_buffer, &render);
    const auto& pipelines =
        operation == draw_operation_e::mix ? owner->drawing->mix_pipelines : owner->drawing->pipelines;
    owner->vk.vkCmdBindPipeline(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[target->format][compositing]);
    owner->vk.vkCmdBindDescriptorSets(command_buffer,
                                      VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      owner->drawing->pipeline_layout,
                                      0,
                                      1,
                                      &descriptor,
                                      0,
                                      nullptr);

    const VkViewport viewport{
        0, 0, static_cast<float>(target->extent.width), static_cast<float>(target->extent.height), 0, 1};
    owner->vk.vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    owner->vk.vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    owner->vk.vkCmdPushConstants(command_buffer,
                                 owner->drawing->pipeline_layout,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                 0,
                                 static_cast<uint32_t>(parameters.size()),
                                 parameters.data());
    owner->vk.vkCmdDraw(command_buffer, 6, 1, 0, 0);
    owner->vk.vkCmdEndRendering(command_buffer);
}
} // namespace miximus::gpu::detail
