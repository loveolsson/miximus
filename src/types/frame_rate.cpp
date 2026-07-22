#include "frame_rate.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace miximus {

namespace {
uint32_t parse_component(const nlohmann::json& json, std::string_view name)
{
    if (!json.is_number_integer()) {
        throw std::invalid_argument(std::format("Frame rate {} must be a positive integer", name));
    }

    if (json.is_number_unsigned()) {
        const auto value = json.get<uint64_t>();
        if (value == 0 || value > std::numeric_limits<uint32_t>::max()) {
            throw std::out_of_range(std::format("Frame rate {} is outside the uint32_t range", name));
        }
        return static_cast<uint32_t>(value);
    }

    const auto value = json.get<int64_t>();
    if (value <= 0 || std::cmp_greater(value, std::numeric_limits<uint32_t>::max())) {
        throw std::out_of_range(std::format("Frame rate {} is outside the uint32_t range", name));
    }
    return static_cast<uint32_t>(value);
}
} // namespace

std::optional<utils::flicks> get_frame_duration(frame_rate_s frame_rate)
{
    if (frame_rate.numerator == 0 || frame_rate.denominator == 0) {
        return std::nullopt;
    }

    constexpr auto FLICKS_PER_SECOND = static_cast<uint64_t>(utils::k_flicks_one_second.count());
    const auto     scaled_duration   = FLICKS_PER_SECOND * frame_rate.denominator;
    if (scaled_duration % frame_rate.numerator != 0) {
        return std::nullopt;
    }

    const auto duration = scaled_duration / frame_rate.numerator;
    if (duration == 0 || duration > static_cast<uint64_t>(std::numeric_limits<utils::flicks::rep>::max())) {
        return std::nullopt;
    }
    return utils::flicks{static_cast<utils::flicks::rep>(duration)};
}

frame_rate_s canonicalize_frame_rate(frame_rate_s frame_rate)
{
    if (!get_frame_duration(frame_rate).has_value()) {
        throw std::invalid_argument("Frame rate cannot be represented exactly in flicks");
    }

    const auto divisor = std::gcd(frame_rate.numerator, frame_rate.denominator);
    frame_rate.numerator /= divisor;
    frame_rate.denominator /= divisor;
    return frame_rate;
}

void to_json(nlohmann::json& json, const frame_rate_s& frame_rate)
{
    json = {
        {"numerator",   frame_rate.numerator  },
        {"denominator", frame_rate.denominator},
    };
}

void from_json(const nlohmann::json& json, frame_rate_s& frame_rate)
{
    if (!json.is_object() || json.size() != 2) {
        throw std::invalid_argument("Frame rate must contain numerator and denominator");
    }

    frame_rate = canonicalize_frame_rate({
        .numerator   = parse_component(json.at("numerator"), "numerator"),
        .denominator = parse_component(json.at("denominator"), "denominator"),
    });
}

} // namespace miximus
