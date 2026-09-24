#pragma once

#include "gpu/device.hpp"
#include "pipeline.hpp"
#include "resource.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace miximus::gpu::detail {

struct arena_s
{
    VkCommandPool                   pool{};
    VkCommandBuffer                 commands{};
    std::vector<VkDescriptorPool>   descriptor_pools;
    VkCommandBuffer                 prologue{};
    std::atomic_bool                reserved{};
    std::atomic<recording_state_s*> queued{};
};

struct submission_state_s
{
    // Zero means queued but not submitted; failure never masquerades as a signal.
    std::atomic_uint64_t value{};
    std::atomic_bool     failed{};
};

struct recording_context_state_s : std::enable_shared_from_this<recording_context_state_s>
{
    std::shared_ptr<device_state_s>       owner;
    std::vector<std::unique_ptr<arena_s>> arenas;
    std::atomic_uint64_t                  next_sequence{};
    uint64_t                              submitted_sequence{}; // Submission worker only.

    ~recording_context_state_s();
    void                               initialize(uint32_t capacity);
    std::unique_ptr<recording_state_s> try_record();
};

struct recording_state_s
{
    std::shared_ptr<device_state_s>                owner;
    std::shared_ptr<recording_context_state_s>     context;
    arena_s*                                       arena{};
    std::vector<std::shared_ptr<resource_state_s>> resources;

    // First-use transitions are recorded by the submission worker in a prologue.
    // The body uses local layouts and can be recorded concurrently with other bodies.
    struct initial_use_s
    {
        VkImageLayout         layout{};
        VkPipelineStageFlags2 stage{};
        VkAccessFlags2        access{};
    };
    std::unordered_map<texture_state_s*, std::vector<initial_use_s>> initial_uses;
    std::unordered_map<texture_state_s*, std::vector<VkImageLayout>> layouts;
    std::unordered_map<texture_state_s*, bool>                       mip_dirty;
    std::unordered_map<texture_state_s*, uint64_t>                   generated_mips;
    std::vector<VkSemaphoreSubmitInfo>                               waits;
    std::vector<completion_s>                                        dependencies;
    std::vector<VkSemaphoreSubmitInfo>                               signals;
    std::vector<std::function<void(completion_s)>>                   publications;
    std::shared_ptr<submission_state_s>                              submission;
    uint64_t                                                         sequence{};
    size_t                                                           descriptor_requests{};
    bool                                                             submission_attempted{};

    recording_state_s(std::shared_ptr<recording_context_state_s> recording_context, arena_s* slot);
    ~recording_state_s();

    void retain(const std::shared_ptr<resource_state_s>& resource);
    void transition(const std::shared_ptr<texture_state_s>& image,
                    VkImageLayout                           layout,
                    VkPipelineStageFlags2                   stage,
                    VkAccessFlags2                          access,
                    uint32_t                                mip = 0);

    VkDescriptorSet allocate_descriptor(VkDescriptorSetLayout layout);
    void            prepare_sampled(const std::shared_ptr<texture_state_s>& image, bool minifying);
    void
    buffer_barrier(const std::shared_ptr<buffer_state_s>& buffer, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    void convert(const std::shared_ptr<buffer_state_s>&  buffer,
                 const std::shared_ptr<texture_state_s>& image,
                 size_t                                  stride,
                 conversion_operation_e                  operation,
                 const color_transform_s&                color = {},
                 channel_order_e                         order = channel_order_e::rgba);
    void draw(const std::shared_ptr<texture_state_s>&      a,
              const std::shared_ptr<texture_state_s>&      b,
              const std::shared_ptr<texture_state_s>&      target,
              std::span<const std::byte>                   parameters,
              compositing_e                                compositing,
              draw_operation_e                             operation,
              std::array<bool, 2>                          minifying,
              const std::optional<std::array<int32_t, 4>>& clip);

    completion_s submit(std::span<const VkSemaphoreSubmitInfo> wait_semaphores   = {},
                        std::span<const VkSemaphoreSubmitInfo> signal_semaphores = {});
    void         record_prologue();
    void         submit_native();
};
} // namespace miximus::gpu::detail
