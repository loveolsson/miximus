#pragma once

#include "resource_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace miximus::gpu {

// Owned diagnostic snapshots. No serialization or native API types are required.

struct queue_diagnostics_s
{
    uint32_t flags{};
    uint32_t count{};
    uint32_t timestamp_bits{};
};

struct format_diagnostics_s
{
    format_e format{};
    uint32_t optimal_features{};
    bool     supported{};
};

struct memory_type_diagnostics_s
{
    uint32_t flags{};
    uint32_t heap{};
    uint64_t heap_bytes{};
};

struct portability_diagnostics_s
{
    bool        image_view_format_swizzle{};
    bool        image_view_format_reinterpretation{};
    bool        events{};
    std::string required_optional_subset_features{};
};

struct physical_device_diagnostics_s
{
    std::string                              name{};
    std::string                              uuid{};
    uint32_t                                 vendor_id{};
    uint32_t                                 device_id{};
    uint32_t                                 api_version{};
    uint32_t                                 driver_version{};
    bool                                     dynamic_rendering{};
    bool                                     synchronization2{};
    bool                                     timeline_semaphores{};
    std::vector<queue_diagnostics_s>         queues{};
    std::vector<format_diagnostics_s>        formats{};
    std::vector<memory_type_diagnostics_s>   memory_types{};
    std::vector<std::string>                 extensions{};
    std::optional<portability_diagnostics_s> portability_subset{};
    bool                                     buffer_conversion{};
    bool                                     swapchain_maintenance{};
    bool                                     supported{};
};

struct external_image_diagnostics_s
{
    bool                     requested{};
    bool                     enabled{};
    std::vector<std::string> enabled_extensions{};
    std::vector<std::string> missing_support{};
};

struct memory_diagnostics_s
{
    uint32_t allocation_count{};
    uint64_t allocation_bytes{};
    uint64_t block_bytes{};
};

struct device_diagnostics_s
{
    std::vector<physical_device_diagnostics_s> devices{};
    bool                                       validation{};
    std::string                                api_floor{};
    std::optional<std::string>                 selected_uuid{};
    external_image_diagnostics_s               external_image_import{};
    bool                                       buffer_conversion{};
    bool                                       separate_present_queue{};
    bool                                       swapchain_maintenance{};
    bool                                       present_wait{};
    bool                                       cuda_external_memory{};
    bool                                       cuda_requested{};
    bool                                       use_cuda{};
    std::optional<bool>                        cuda_transfer_formats_qualified{};
    std::optional<std::vector<std::string>>    cuda_missing_support{};
    std::optional<memory_diagnostics_s>        memory{};
};

} // namespace miximus::gpu
