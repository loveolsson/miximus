#include "core/app_state.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "register.hpp"
#include "types/frame_rate.hpp"
#include "types/output_buffer_limits.hpp"

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

using framebuffer_settings_s = core::app_state_s::frame_settings_s::framebuffer_settings_s;

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

option_result_e normalize_framebuffer_size(json* value)
{
    if (value == nullptr || !value->is_array() || value->size() != 2) {
        return option_result_e::invalid;
    }

    const auto x_result = normalize_option_value<int>(
        &value->at(0), framebuffer_settings_s::MIN_DIMENSION, framebuffer_settings_s::MAX_DIMENSION);
    const auto y_result = normalize_option_value<int>(
        &value->at(1), framebuffer_settings_s::MIN_DIMENSION, framebuffer_settings_s::MAX_DIMENSION);
    return combine_option_results(x_result, y_result);
}

class node_impl final : public node_i
{
  public:
    std::string_view type() const final { return miximus::nodes::system::SETTINGS_NODE_TYPE; }

    void execute(core::app_state_s* /*app*/, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"frame_rate",                    DEFAULT_FRAME_RATE                                       },
            {"default_framebuffer_size",
             gpu::vec2_t{framebuffer_settings_s::DEFAULT_WIDTH, framebuffer_settings_s::DEFAULT_HEIGHT}},
            {"decklink_output_buffer_frames", decklink_output_buffer_limits_s::DEFAULT_FRAME_COUNT     },
            {"ndi_output_buffer_frames",      ndi_output_buffer_limits_s::DEFAULT_FRAME_COUNT          },
            {"screen_output_buffer_frames",   screen_output_buffer_limits_s::DEFAULT_FRAME_COUNT       },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "frame_rate") {
            return normalize_frame_rate(value);
        }
        if (name == "default_framebuffer_size") {
            return normalize_framebuffer_size(value);
        }
        if (name == "decklink_output_buffer_frames") {
            return normalize_option_value<int>(value,
                                               decklink_output_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               decklink_output_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "ndi_output_buffer_frames") {
            return normalize_option_value<int>(value,
                                               ndi_output_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               ndi_output_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "screen_output_buffer_frames") {
            return normalize_option_value<int>(value,
                                               screen_output_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               screen_output_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        return option_result_e::invalid;
    }
};

} // namespace

namespace miximus::nodes::system {

std::shared_ptr<node_i> create_settings_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::system
