#pragma once

#include "gpu/device.hpp"
#include "utils/lookup.hpp"

#include <volk.h>

namespace miximus::gpu::detail {

enum class draw_operation_e
{
    texture,
    mix,
};

// Conversion pipelines are indexed by operation, independently of the shader filename.
enum class conversion_operation_e
{
    unpack_v210,
    pack_v210,
};

struct pipeline_state_s
{
    device_state_s& owner;
    explicit pipeline_state_s(device_state_s& device)
        : owner(device)
    {
    }
    ~pipeline_state_s();
    pipeline_state_s(const pipeline_state_s&)            = delete;
    pipeline_state_s& operator=(const pipeline_state_s&) = delete;
    pipeline_state_s(pipeline_state_s&&)                 = delete;
    pipeline_state_s& operator=(pipeline_state_s&&)      = delete;
    void              initialize();
    // Immutable pipelines and layouts are warmed before the render loop.
    using graphics_pipelines_t = enum_array_t<format_e, enum_array_t<compositing_e, VkPipeline>>;
    graphics_pipelines_t  pipelines{};
    graphics_pipelines_t  mix_pipelines{};
    VkDescriptorSetLayout texture_layout{};
    VkPipelineLayout      pipeline_layout{};
    VkSampler             sampler{};
    VkSampler             nearest_sampler{};

    enum_array_t<conversion_operation_e, VkPipeline> conversion_pipelines{};
    VkDescriptorSetLayout                            conversion_layout{};
    VkPipelineLayout                                 conversion_pipeline_layout{};
};
} // namespace miximus::gpu::detail
