#pragma once

#include "gpu/device.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace miximus::gpu::detail {

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

} // namespace miximus::gpu::detail
