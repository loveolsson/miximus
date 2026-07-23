#include "core/app_state.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "register.hpp"
#include "types/frame_rate.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using nlohmann::json;

using decklink_output_settings_s = core::app_state_s::frame_settings_s::decklink_output_settings_s;
using ndi_output_settings_s      = core::app_state_s::frame_settings_s::ndi_output_settings_s;

std::optional<uint32_t> read_positive_uint32(const json& value)
{
    if (!value.is_number_integer()) {
        return std::nullopt;
    }

    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<uint64_t>();
        if (unsigned_value == 0 || unsigned_value > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(unsigned_value);
    }

    const auto signed_value = value.get<int64_t>();
    if (signed_value <= 0 || std::cmp_greater(signed_value, std::numeric_limits<uint32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(signed_value);
}

option_result_e normalize_frame_rate(json* value)
{
    if (value == nullptr || !value->is_object() || value->size() != 2) {
        return option_result_e::invalid;
    }

    const auto numerator_it   = value->find("numerator");
    const auto denominator_it = value->find("denominator");
    if (numerator_it == value->end() || denominator_it == value->end()) {
        return option_result_e::invalid;
    }

    const auto numerator   = read_positive_uint32(*numerator_it);
    const auto denominator = read_positive_uint32(*denominator_it);
    if (!numerator.has_value() || !denominator.has_value()) {
        return option_result_e::invalid;
    }

    const frame_rate_s input{
        .numerator   = *numerator,
        .denominator = *denominator,
    };
    if (!get_frame_duration(input).has_value()) {
        return option_result_e::invalid;
    }

    const auto normalized = canonicalize_frame_rate(input);
    *value                = normalized;
    return normalized == input ? option_result_e::ok : option_result_e::corrected;
}

class node_impl final : public node_i
{
  public:
    std::string_view type() const final { return miximus::nodes::system::SETTINGS_NODE_TYPE; }

    void execute(core::app_state_s* /*app*/, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"frame_rate",                     DEFAULT_FRAME_RATE                                },
            {"decklink_output_preroll_frames", decklink_output_settings_s::DEFAULT_PREROLL_FRAMES},
            {"decklink_output_buffer_frames",  decklink_output_settings_s::DEFAULT_BUFFER_FRAMES },
            {"ndi_output_buffer_frames",       ndi_output_settings_s::DEFAULT_BUFFER_FRAMES      },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "frame_rate") {
            return normalize_frame_rate(value);
        }
        if (name == "decklink_output_preroll_frames" || name == "decklink_output_buffer_frames") {
            return normalize_option_value<int>(
                value, decklink_output_settings_s::MIN_BUFFER_FRAMES, decklink_output_settings_s::MAX_BUFFER_FRAMES);
        }
        if (name == "ndi_output_buffer_frames") {
            return normalize_option_value<int>(
                value, ndi_output_settings_s::MIN_BUFFER_FRAMES, ndi_output_settings_s::MAX_BUFFER_FRAMES);
        }
        return option_result_e::invalid;
    }
};

} // namespace

namespace miximus::nodes::system {

std::shared_ptr<node_i> create_settings_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::system
