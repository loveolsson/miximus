#include "device_diagnostics_json.hpp"

#include "utils/lookup.hpp"

#include <nlohmann/json.hpp>

namespace miximus::gpu {

// Nested serializers need this namespace for ADL, but no external linkage.
// ADL ignores an anonymous namespace's implicit using-directive.
// NOLINTBEGIN(misc-use-anonymous-namespace)
static void to_json(nlohmann::json& json, const queue_diagnostics_s& value)
{
    json = {
        {"flags",          value.flags         },
        {"count",          value.count         },
        {"timestamp_bits", value.timestamp_bits},
    };
}

static void to_json(nlohmann::json& json, const format_diagnostics_s& value)
{
    json = {
        {"format",           enum_to_string(value.format)},
        {"optimal_features", value.optimal_features      },
        {"supported",        value.supported             },
    };
}

static void to_json(nlohmann::json& json, const memory_type_diagnostics_s& value)
{
    json = {
        {"flags",      value.flags     },
        {"heap",       value.heap      },
        {"heap_bytes", value.heap_bytes},
    };
}

static void to_json(nlohmann::json& json, const portability_diagnostics_s& value)
{
    json = {
        {"image_view_format_swizzle",          value.image_view_format_swizzle         },
        {"image_view_format_reinterpretation", value.image_view_format_reinterpretation},
        {"events",                             value.events                            },
        {"required_optional_subset_features",  value.required_optional_subset_features },
    };
}

static void to_json(nlohmann::json& json, const physical_device_diagnostics_s& value)
{
    json = {
        {"name",                  value.name                 },
        {"uuid",                  value.uuid                 },
        {"vendor_id",             value.vendor_id            },
        {"device_id",             value.device_id            },
        {"api_version",           value.api_version          },
        {"driver_version",        value.driver_version       },
        {"dynamic_rendering",     value.dynamic_rendering    },
        {"synchronization2",      value.synchronization2     },
        {"timeline_semaphores",   value.timeline_semaphores  },
        {"queues",                value.queues               },
        {"formats",               value.formats              },
        {"memory_types",          value.memory_types         },
        {"extensions",            value.extensions           },
        {"buffer_conversion",     value.buffer_conversion    },
        {"swapchain_maintenance", value.swapchain_maintenance},
        {"supported",             value.supported            },
    };

    if (value.portability_subset.has_value()) {
        json["portability_subset"] = *value.portability_subset;
    }
}

static void to_json(nlohmann::json& json, const external_image_diagnostics_s& value)
{
    json = {
        {"requested",          value.requested         },
        {"enabled",            value.enabled           },
        {"enabled_extensions", value.enabled_extensions},
        {"missing_support",    value.missing_support   },
    };
}

static void to_json(nlohmann::json& json, const memory_diagnostics_s& value)
{
    json = {
        {"allocation_count", value.allocation_count},
        {"allocation_bytes", value.allocation_bytes},
        {"block_bytes",      value.block_bytes     },
    };
}

// NOLINTEND(misc-use-anonymous-namespace)

void to_json(nlohmann::json& json, const device_diagnostics_s& value)
{
    json = {
        {"devices",                value.devices               },
        {"validation",             value.validation            },
        {"api_floor",              value.api_floor             },
        {"external_image_import",  value.external_image_import },
        {"buffer_conversion",      value.buffer_conversion     },
        {"separate_present_queue", value.separate_present_queue},
        {"swapchain_maintenance",  value.swapchain_maintenance },
        {"present_wait",           value.present_wait          },
        {"cuda_external_memory",   value.cuda_external_memory  },
        {"cuda_requested",         value.cuda_requested        },
        {"use_cuda",               value.use_cuda              },
    };

    if (value.selected_uuid.has_value()) {
        json["selected_uuid"] = *value.selected_uuid;
    }

    if (value.cuda_transfer_formats_qualified.has_value()) {
        json["cuda_transfer_formats_qualified"] = *value.cuda_transfer_formats_qualified;
    }

    if (value.cuda_missing_support.has_value()) {
        json["cuda_missing_support"] = *value.cuda_missing_support;
    }

    if (value.memory.has_value()) {
        json["memory"] = *value.memory;
    }
}

std::string format_device_diagnostics(const device_diagnostics_s& value) { return nlohmann::json(value).dump(2); }

} // namespace miximus::gpu
