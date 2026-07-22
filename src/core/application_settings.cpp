#include "application_settings.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>

namespace {
using namespace miximus;
using nlohmann::json;

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

nodes::option_result_e normalize_frame_rate(json* value)
{
    if (value == nullptr || !value->is_object() || value->size() != 2) {
        return nodes::option_result_e::invalid;
    }

    const auto numerator_it   = value->find("numerator");
    const auto denominator_it = value->find("denominator");
    if (numerator_it == value->end() || denominator_it == value->end()) {
        return nodes::option_result_e::invalid;
    }

    const auto numerator   = read_positive_uint32(*numerator_it);
    const auto denominator = read_positive_uint32(*denominator_it);
    if (!numerator.has_value() || !denominator.has_value()) {
        return nodes::option_result_e::invalid;
    }

    const frame_rate_s input{
        .numerator   = *numerator,
        .denominator = *denominator,
    };
    if (!get_frame_duration(input).has_value()) {
        return nodes::option_result_e::invalid;
    }

    const auto normalized = canonicalize_frame_rate(input);
    *value                = normalized;
    return normalized == input ? nodes::option_result_e::ok : nodes::option_result_e::corrected;
}
} // namespace

namespace miximus::core {

class application_settings_s::impl_s
{
  public:
    nlohmann::json options{
        {"frame_rate", DEFAULT_FRAME_RATE}
    };
    frame_rate_s render_frame_rate{DEFAULT_FRAME_RATE};
    uint64_t     revision{};
    bool         dirty{true};
};

application_settings_s::application_settings_s()
    : impl_(std::make_unique<impl_s>())
{
}

application_settings_s::~application_settings_s() = default;

nodes::option_result_e application_settings_s::normalize_option(std::string_view name, nlohmann::json* value)
{
    if (name == "frame_rate") {
        return normalize_frame_rate(value);
    }
    return nodes::option_result_e::invalid;
}

nodes::set_options_result_s application_settings_s::set_options(const nlohmann::json& options)
{
    if (!options.is_object()) {
        return {
            .error                = error_e::invalid_options,
            .has_corrected_values = false,
        };
    }

    auto normalized_options   = impl_->options;
    bool has_corrected_values = false;
    for (auto option = options.begin(); option != options.end(); ++option) {
        auto value  = option.value();
        auto result = normalize_option(option.key(), &value);
        if (result == nodes::option_result_e::invalid) {
            return {
                .error                = error_e::invalid_options,
                .has_corrected_values = false,
            };
        }
        has_corrected_values |= result == nodes::option_result_e::corrected;
        normalized_options[option.key()] = std::move(value);
    }

    if (normalized_options != impl_->options) {
        impl_->options = std::move(normalized_options);
        impl_->dirty   = true;
    }
    return {
        .error                = error_e::no_error,
        .has_corrected_values = has_corrected_values,
    };
}

const nlohmann::json& application_settings_s::options() const { return impl_->options; }

application_settings_snapshot_s application_settings_s::sync_render_snapshot()
{
    if (impl_->dirty) {
        impl_->render_frame_rate = impl_->options.at("frame_rate").get<frame_rate_s>();
        ++impl_->revision;
        impl_->dirty = false;
    }
    return {
        .frame_rate = impl_->render_frame_rate,
        .revision   = impl_->revision,
    };
}

} // namespace miximus::core
