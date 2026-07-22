#pragma once
#include "utils/flicks.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>

namespace miximus {

struct frame_rate_s
{
    uint32_t numerator{60};
    uint32_t denominator{1};

    auto operator<=>(const frame_rate_s&) const = default;
};

constexpr frame_rate_s DEFAULT_FRAME_RATE{};

std::optional<utils::flicks> get_frame_duration(frame_rate_s frame_rate);
frame_rate_s                 canonicalize_frame_rate(frame_rate_s frame_rate);

void to_json(nlohmann::json& json, const frame_rate_s& frame_rate);
void from_json(const nlohmann::json& json, frame_rate_s& frame_rate);

} // namespace miximus
