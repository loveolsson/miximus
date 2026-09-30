#include "transfer_report.hpp"

#include "gpu/device_diagnostics_json.hpp"

#include <nlohmann/json.hpp>

namespace miximus::gpu {

void to_json(nlohmann::json& json, const transfer_measurement_s& value)
{
    json = {
        {"direction",               value.direction              },
        {"backend",                 value.backend                },
        {"samples",                 value.samples                },
        {"mean_us",                 value.mean_us                },
        {"p50_us",                  value.p50_us                 },
        {"p95_us",                  value.p95_us                 },
        {"p99_us",                  value.p99_us                 },
        {"effective_GB_per_second", value.effective_gb_per_second},
        {"width",                   value.width                  },
        {"height",                  value.height                 },
        {"format",                  value.format                 },
        {"bytes",                   value.bytes                  },
        {"row_stride",              value.row_stride             },
        {"pixels_verified",         value.pixels_verified        },
    };
}

void to_json(nlohmann::json& json, const transfer_report_s& value)
{
    // Preserve the benchmark's existing flat output schema at this boundary.
    to_json(json, value.device);
    json["method"]            = value.method;
    json["results"]           = value.results;
    json["validation_errors"] = value.validation_errors;
}

} // namespace miximus::gpu
