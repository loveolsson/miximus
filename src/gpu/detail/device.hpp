#pragma once

#include "fatal.hpp"
#include "gpu/device.hpp"
#include "utils/lookup.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace miximus::gpu::detail {

void         check(VkResult result, const char* operation);
completion_s enqueue_recording(std::unique_ptr<recording_state_s>&    recording,
                               std::span<const VkSemaphoreSubmitInfo> waits   = {},
                               std::span<const VkSemaphoreSubmitInfo> signals = {});

VkFormat native_format(format_e format);
uint32_t texel_bytes(format_e format);

enum class draw_operation_e
{
    texture,
    mix,
};

// Conversion pipelines are indexed by operation, independently of the shader filename.
enum class conversion_operation_e
{
    unpack_rgba,
    pack_rgba,
    unpack_v210,
    pack_v210,
};

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

struct retired_s
{
    uint64_t              completion{};
    std::function<void()> destroy;
};

struct device_state_s : std::enable_shared_from_this<device_state_s>
{
    // Native dispatch and allocation state live as long as any resource retains this owner.
    VolkInstanceTable        instance_vk{};
    VkInstance               instance{};
    VkDebugUtilsMessengerEXT debug{};
    VkPhysicalDevice         physical{};
    VkDevice                 device{};
    VolkDeviceTable          vk{};
    VmaAllocator             allocator{};

    VkPhysicalDeviceProperties       properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    bool                             separate_present_queue{};
    bool                             swapchain_maintenance{};
    bool                             present_wait{};
    bool                             cuda_external_memory{};
    int                              cuda_device_index{-1};
    std::vector<std::string>         cuda_missing_support;
    bool                             buffer_conversion{};

    // Only the submission worker calls vkQueueSubmit. Present shares its queue
    // mutex solely on devices without a second queue; recorders never take it.
    VkQueue    queue{};
    VkQueue    present_queue{};
    uint32_t   queue_family{};
    std::mutex queue_mutex;
    std::mutex present_mutex;

    VkSemaphore                                           timeline{};
    uint64_t                                              last_submitted_timeline_value{};
    std::atomic_uint64_t                                  completed_value{};
    std::mutex                                            contexts_mutex;
    std::vector<std::weak_ptr<recording_context_state_s>> contexts;
    std::atomic_bool                                      submission_failed{};
    std::thread                                           submission_worker;
    std::atomic_bool                                      stopping{};
    std::mutex                                            wake_mutex;
    std::condition_variable                               wake;

    void                   start_submission_worker();
    void                   stop_submission_worker();
    void                   run_submissions();
    std::mutex             retire_mutex;
    std::vector<retired_s> retired;

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

    std::atomic_uint64_t errors{};
    device_options_s     options;
    std::string          diagnostics;

    ~device_state_s();

    uint64_t completed() const;
    void     retire(uint64_t after, std::function<void()> destroy);
    void     collect();

    void                     initialize(const device_options_s& configuration);
    bool                     initialize_instance();
    std::vector<const char*> select_cuda_extensions(std::span<const uint8_t, VK_UUID_SIZE> uuid,
                                                    std::span<const VkExtensionProperties> extensions);
    std::vector<const char*> select_physical_device(bool surface_maintenance_available);
    void                     initialize_logical_device(std::span<const char* const> device_extensions);
    void                     initialize_allocator();
    void                     initialize_submission_timeline();
    void                     initialize_pipelines();
};

struct resource_state_s
{
    std::shared_ptr<device_state_s> owner;

    // CPU leases and recorded GPU uses are separate reasons an allocation remains live.
    std::atomic_uint64_t last_use_timeline_value{};
    std::atomic_uint32_t recording_uses{};
    void                 check_host_access() const;
};

struct texture_state_s : resource_state_s
{
    VkImage                    image{};
    VkImageView                view{};
    VkImageView                sampled_view{};
    VmaAllocation              allocation{};
    VkDeviceMemory             external_memory{};
    size_t                     external_allocation_bytes{};
    extent_s                   extent;
    format_e                   format{};
    sampling_e                 sampling{};
    uint32_t                   mip_levels{1};
    std::vector<VkImageLayout> layouts;
    std::atomic_uint64_t       content_version{};
    std::atomic_uint64_t       mip_version{UINT64_MAX};

    ~texture_state_s();
};

struct buffer_state_s : resource_state_s
{
    VkBuffer          buffer{};
    VmaAllocation     allocation{};
    VkDeviceMemory    external_memory{};
    bool              external_buffer{};
    void*             mapped{};
    size_t            bytes{};
    host_access_e     access{};
    allocation_info_s info;
    std::atomic_bool  host_dirty{};

    void flush_host_writes();
    ~buffer_state_s();
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
