#pragma once

#include "gpu/device_diagnostics.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>

namespace miximus::gpu {

struct transfer_measurement_s
{
    std::string direction;
    std::string backend;
    size_t      samples{};
    double      mean_us{};
    double      p50_us{};
    double      p95_us{};
    double      p99_us{};
    double      effective_gb_per_second{};
    int         width{};
    int         height{};
    std::string format;
    size_t      bytes{};
    size_t      row_stride{};
    bool        pixels_verified{};
};

struct transfer_report_s
{
    device_diagnostics_s                device;
    std::string                         method;
    std::vector<transfer_measurement_s> results;
    uint64_t                            validation_errors{};
};

void to_json(nlohmann::json& json, const transfer_measurement_s& value);
void to_json(nlohmann::json& json, const transfer_report_s& value);

} // namespace miximus::gpu
