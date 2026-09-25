#include "color_compare.hpp"

#include "gpu/detail/device.hpp"
#include "gpu/detail/recording.hpp"
#include "gpu/detail/resource.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace miximus::gpu::detail {

struct color_comparison_s::state_s : resource_state_s
{
    VkShaderModule        shader{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout      layout{};
    VkPipeline            pipeline{};

    state_s()                                = default;
    state_s(const state_s& other)            = delete;
    state_s& operator=(const state_s& other) = delete;
    state_s(state_s&& other)                 = delete;
    state_s& operator=(state_s&& other)      = delete;

    ~state_s()
    {
        if (!owner) {
            return;
        }

        owner->retire(last_use_timeline_value.load(),
                      [device      = owner->device,
                       vk          = owner->vk,
                       shader      = shader,
                       descriptors = descriptors,
                       layout      = layout,
                       pipeline    = pipeline] {
                          if (pipeline != VK_NULL_HANDLE) {
                              vk.vkDestroyPipeline(device, pipeline, nullptr);
                          }

                          if (layout != VK_NULL_HANDLE) {
                              vk.vkDestroyPipelineLayout(device, layout, nullptr);
                          }

                          if (descriptors != VK_NULL_HANDLE) {
                              vk.vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
                          }

                          if (shader != VK_NULL_HANDLE) {
                              vk.vkDestroyShaderModule(device, shader, nullptr);
                          }
                      });
    }
};

color_comparison_s::color_comparison_s(const texture_s& source, const std::filesystem::path& shader_path)
    : state_(std::make_shared<state_s>())
{
    if (!source.state_) {
        throw std::invalid_argument("Color comparison needs an owned source image");
    }

    state_->owner = source.state_->owner;
    std::ifstream input(shader_path, std::ios::binary | std::ios::ate);
    const auto    bytes = input.tellg();
    if (!input || bytes <= 0 || bytes % 4 != 0) {
        throw std::runtime_error("Cannot read color comparison shader");
    }

    std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(code.data()), bytes)) {
        throw std::runtime_error("Incomplete color comparison shader");
    }

    const auto  device = state_->owner->device;
    const auto& vk     = state_->owner->vk;

    VkShaderModuleCreateInfo shader{};
    shader.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = static_cast<size_t>(bytes);
    shader.pCode    = code.data();

    check(vk.vkCreateShaderModule(device, &shader, nullptr, &state_->shader), "create comparison shader");
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        {
         {.binding            = 0,
             .descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount    = 1,
             .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
             .pImmutableSamplers = nullptr},
         {.binding            = 1,
             .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .descriptorCount    = 1,
             .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
             .pImmutableSamplers = nullptr},
         }
    };

    VkDescriptorSetLayoutCreateInfo descriptors{};
    descriptors.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptors.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptors.pBindings    = bindings.data();

    check(vk.vkCreateDescriptorSetLayout(device, &descriptors, nullptr, &state_->descriptors),
          "create comparison descriptors");
    const VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28};

    VkPipelineLayoutCreateInfo layout{};
    layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount         = 1;
    layout.pSetLayouts            = &state_->descriptors;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &constants;

    check(vk.vkCreatePipelineLayout(device, &layout, nullptr, &state_->layout), "create comparison layout");

    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = state_->shader;
    pipeline.stage.pName  = "main";
    pipeline.layout       = state_->layout;

    check(vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &state_->pipeline),
          "create comparison pipeline");
}

color_comparison_s::~color_comparison_s() = default;

void color_comparison_s::record(recording_s&         recording,
                                const texture_s&     source,
                                const buffer_s&      counters,
                                std::array<float, 4> reference,
                                float                tolerance,
                                uint32_t             x_begin,
                                uint32_t             x_end)
{
    if (!recording.state_ || !source.state_ || !counters.state_ || counters.size() != 8 || tolerance < 0) {
        throw std::invalid_argument("Invalid color comparison resources");
    }

    if (x_end == 0U) {
        x_end = source.extent().width;
    }

    if (x_begin >= x_end || x_end > source.extent().width) {
        throw std::invalid_argument("Invalid comparison horizontal range");
    }

    auto& state = *recording.state_;
    if (state.owner != state_->owner || source.state_->owner != state.owner || counters.state_->owner != state.owner) {
        throw std::invalid_argument("Color comparison resources must share the recording device");
    }

    state.retain(state_);
    state.retain(counters.state_);
    state.buffer_barrier(counters.state_,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    state.transition(source.state_,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const auto                  descriptor = state.allocate_descriptor(state_->descriptors);
    const VkDescriptorImageInfo image{
        state.owner->drawing->nearest_sampler, source.state_->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo        buffer{counters.state_->buffer, 0, 8};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (auto& write : writes) {
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = descriptor;
        write.descriptorCount = 1;
    }

    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo     = &image;
    writes[1].dstBinding     = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo    = &buffer;
    const auto& vk           = state.owner->vk;

    vk.vkUpdateDescriptorSets(state.owner->device, 2, writes.data(), 0, nullptr);

    vk.vkCmdBindPipeline(state.arena->commands, VK_PIPELINE_BIND_POINT_COMPUTE, state_->pipeline);

    vk.vkCmdBindDescriptorSets(
        state.arena->commands, VK_PIPELINE_BIND_POINT_COMPUTE, state_->layout, 0, 1, &descriptor, 0, nullptr);
    struct parameters_s
    {
        std::array<float, 4> reference;
        float                tolerance;
        uint32_t             x_begin;
        uint32_t             x_end;
    } parameters{.reference = reference, .tolerance = tolerance, .x_begin = x_begin, .x_end = x_end};
    static_assert(sizeof(parameters) == 28);

    vk.vkCmdPushConstants(
        state.arena->commands, state_->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    const auto extent = source.extent();

    vk.vkCmdDispatch(state.arena->commands, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);
    state.buffer_barrier(counters.state_, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
} // namespace miximus::gpu::detail
