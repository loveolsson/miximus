#include "device.hpp"
#include "recording.hpp"
#include "resource.hpp"
#include "static_files/files.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace miximus::gpu::detail {
namespace {

struct shader_module_s
{
    device_state_s& owner;

    VkShaderModule module{};
    shader_module_s(device_state_s& device, const char* name)
        : owner(device)
    {
        const auto bytes = static_files::get_shader_files().get_file_or_throw(std::format("{}.spv", name)).unzip();
        if (bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) {
            throw std::runtime_error(std::format("Invalid SPIR-V byte count for {}", name));
        }

        // Bundled data is byte-oriented; Vulkan requires aligned native uint32_t words.
        std::vector<uint32_t> code(bytes.size() / sizeof(uint32_t));
        for (size_t i = 0; i < code.size(); ++i) {
            for (size_t byte = 0; byte < sizeof(uint32_t); ++byte) {
                code[i] |= uint32_t{static_cast<uint8_t>(bytes[(i * sizeof(uint32_t)) + byte])} << (byte * 8);
            }
        }

        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes.size();
        info.pCode    = code.data();
        check(owner.vk.vkCreateShaderModule(owner.device, &info, nullptr, &module), "shader module");
    }

    shader_module_s(const shader_module_s&)            = delete;
    shader_module_s& operator=(const shader_module_s&) = delete;

    shader_module_s(shader_module_s&&)            = delete;
    shader_module_s& operator=(shader_module_s&&) = delete;

    ~shader_module_s()
    {
        if (module != nullptr) {
            owner.vk.vkDestroyShaderModule(owner.device, module, nullptr);
        }
    }
};
} // namespace

void pipeline_state_s::initialize()
{
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        {{.binding            = 0,
          .descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount    = 1,
          .stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT,
          .pImmutableSamplers = nullptr},
         {.binding            = 1,
          .descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount    = 1,
          .stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT,
          .pImmutableSamplers = nullptr}}
    };

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings    = bindings.data();
    check(owner.vk.vkCreateDescriptorSetLayout(owner.device, &layout, nullptr, &texture_layout),
          "texture descriptor layout");

    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};

    VkPipelineLayoutCreateInfo pipeline_info{};
    pipeline_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount         = 1;
    pipeline_info.pSetLayouts            = &texture_layout;
    pipeline_info.pushConstantRangeCount = 1;
    pipeline_info.pPushConstantRanges    = &push;
    check(owner.vk.vkCreatePipelineLayout(owner.device, &pipeline_info, nullptr, &pipeline_layout), "pipeline layout");

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter    = VK_FILTER_LINEAR;
    sampler_info.minFilter    = VK_FILTER_LINEAR;
    sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod       = VK_LOD_CLAMP_NONE;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(owner.vk.vkCreateSampler(owner.device, &sampler_info, nullptr, &sampler), "sampler");
    sampler_info.magFilter  = VK_FILTER_NEAREST;
    sampler_info.minFilter  = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    check(owner.vk.vkCreateSampler(owner.device, &sampler_info, nullptr, &nearest_sampler), "nearest sampler");
    const shader_module_s                          vertex(owner, "quad.vert");
    const shader_module_s                          fragment(owner, "texture.frag");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }

    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.module;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.module;

    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.lineWidth   = 1;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates    = dynamic_states.data();

    VkPipelineColorBlendAttachmentState blend{};
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blend;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = static_cast<uint32_t>(stages.size());
    info.pStages             = stages.data();
    info.pVertexInputState   = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pColorBlendState    = &blending;
    info.pDynamicState       = &dynamic;
    info.layout              = pipeline_layout;
    // Integer storage is a conversion resource, never a graphics attachment here.
    for (const auto format : {format_e::rgba_unorm8, format_e::rgba_unorm16}) {
        const auto         attachment_format = native_format(format);
        VkFormatProperties features{};
        owner.instance_vk.vkGetPhysicalDeviceFormatProperties(owner.physical, attachment_format, &features);
        if ((features.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0U) {
            continue;
        }
        rendering.pColorAttachmentFormats = &attachment_format;
        for (const auto compositing : {compositing_e::replace, compositing_e::source_over}) {
            blend.blendEnable = static_cast<VkBool32>(compositing == compositing_e::source_over);
            check(owner.vk.vkCreateGraphicsPipelines(
                      owner.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[format][compositing]),
                  "graphics pipeline");
        }
    }

    const shader_module_s mix_fragment(owner, "mix.frag");
    stages[1].module = mix_fragment.module;
    // Integer storage is a conversion resource, never a graphics attachment here.
    for (const auto format : {format_e::rgba_unorm8, format_e::rgba_unorm16}) {
        const auto         attachment_format = native_format(format);
        VkFormatProperties features{};
        owner.instance_vk.vkGetPhysicalDeviceFormatProperties(owner.physical, attachment_format, &features);
        if ((features.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0U) {
            continue;
        }
        rendering.pColorAttachmentFormats = &attachment_format;
        for (const auto compositing : {compositing_e::replace, compositing_e::source_over}) {
            blend.blendEnable = static_cast<VkBool32>(compositing == compositing_e::source_over);
            check(owner.vk.vkCreateGraphicsPipelines(
                      owner.device, VK_NULL_HANDLE, 1, &info, nullptr, &mix_pipelines[format][compositing]),
                  "graphics pipeline");
        }
    }

    if (owner.buffer_conversion) {
        const std::array<VkDescriptorSetLayoutBinding, 2> conversion_bindings{
            {{.binding            = 0,
              .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .descriptorCount    = 1,
              .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = nullptr},
             {.binding            = 1,
              .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount    = 1,
              .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = nullptr}}
        };

        layout.bindingCount = static_cast<uint32_t>(conversion_bindings.size());
        layout.pBindings    = conversion_bindings.data();
        check(owner.vk.vkCreateDescriptorSetLayout(owner.device, &layout, nullptr, &conversion_layout),
              "conversion descriptor layout");

        const VkPushConstantRange conversion_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        pipeline_info.pSetLayouts         = &conversion_layout;
        pipeline_info.pPushConstantRanges = &conversion_push;
        check(owner.vk.vkCreatePipelineLayout(owner.device, &pipeline_info, nullptr, &conversion_pipeline_layout),
              "conversion pipeline layout");
        constexpr std::array conversions{
            std::pair{conversion_operation_e::unpack_v210, "unpack_v210.comp"},
            std::pair{conversion_operation_e::pack_v210,   "pack_v210.comp"  },
        };

        for (const auto& [operation, name] : conversions) {
            const shader_module_s shader(owner, name);

            VkComputePipelineCreateInfo compute{};
            compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            compute.stage.module = shader.module;
            compute.stage.pName  = "main";
            compute.layout       = conversion_pipeline_layout;
            check(owner.vk.vkCreateComputePipelines(
                      owner.device, VK_NULL_HANDLE, 1, &compute, nullptr, &conversion_pipelines[operation]),
                  "conversion pipeline");
        }
    }
}

pipeline_state_s::~pipeline_state_s()
{
    for (const auto& formats : pipelines) {
        for (auto pipeline : formats) {
            if (pipeline != nullptr) {
                owner.vk.vkDestroyPipeline(owner.device, pipeline, nullptr);
            }
        }
    }

    for (const auto& formats : mix_pipelines) {
        for (auto pipeline : formats) {
            if (pipeline != nullptr) {
                owner.vk.vkDestroyPipeline(owner.device, pipeline, nullptr);
            }
        }
    }

    for (auto pipeline : conversion_pipelines) {
        if (pipeline != nullptr) {
            owner.vk.vkDestroyPipeline(owner.device, pipeline, nullptr);
        }
    }

    if (conversion_pipeline_layout != nullptr) {
        owner.vk.vkDestroyPipelineLayout(owner.device, conversion_pipeline_layout, nullptr);
    }

    if (conversion_layout != nullptr) {
        owner.vk.vkDestroyDescriptorSetLayout(owner.device, conversion_layout, nullptr);
    }

    if (sampler != nullptr) {
        owner.vk.vkDestroySampler(owner.device, sampler, nullptr);
    }

    if (nearest_sampler != nullptr) {
        owner.vk.vkDestroySampler(owner.device, nearest_sampler, nullptr);
    }

    if (pipeline_layout != nullptr) {
        owner.vk.vkDestroyPipelineLayout(owner.device, pipeline_layout, nullptr);
    }

    if (texture_layout != nullptr) {
        owner.vk.vkDestroyDescriptorSetLayout(owner.device, texture_layout, nullptr);
    }
}
} // namespace miximus::gpu::detail
