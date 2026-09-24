#pragma once

#include "fatal.hpp"
#include "gpu/device.hpp"
#include "pipeline.hpp"
#include "retirement.hpp"
#include "submission.hpp"

#include <atomic>
#include <functional>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace miximus::gpu::detail {

void         check(VkResult result, const char* operation);
completion_s enqueue_recording(std::unique_ptr<recording_state_s>&    recording,
                               std::span<const VkSemaphoreSubmitInfo> waits   = {},
                               std::span<const VkSemaphoreSubmitInfo> signals = {});

VkFormat native_format(format_e format);
uint32_t texel_bytes(format_e format);

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
    external_image_import_support_s  external_image_import;

    uint32_t            queue_family{};
    submission_engine_s submissions{*this};
    retirement_queue_s  retirement;

    std::unique_ptr<pipeline_state_s> drawing;

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
    void                     enable_external_image_import(std::span<const VkExtensionProperties> selected_extensions,
                                                          std::vector<const char*>&              device_extensions);
    void                     initialize_logical_device(std::span<const char* const> device_extensions);
    void                     initialize_allocator();
};

} // namespace miximus::gpu::detail
