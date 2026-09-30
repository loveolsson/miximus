#pragma once

#include "device_diagnostics.hpp"

#include <nlohmann/json_fwd.hpp>

namespace miximus::gpu {

void to_json(nlohmann::json& json, const device_diagnostics_s& value);

std::string format_device_diagnostics(const device_diagnostics_s& value);

} // namespace miximus::gpu
