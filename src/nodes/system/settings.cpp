#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "nodes/action.hpp"
#include "types/node_action_contracts.hpp"
#if MIXIMUS_ENABLE_CEF
#include "nodes/cef/subsystem.hpp"
#endif
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "register.hpp"
#include "types/buffer_limits.hpp"
#include "types/frame_rate.hpp"
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"

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
    utils::observed_value_s<bool> reported_cache_pending_;

    static void clear_browser_cache(core::app_state_s* app, action_s action)
    {
#if MIXIMUS_ENABLE_CEF
        if (auto* cef = app->cef_subsystem()) {
            if (!cef->clear_http_cache()) {
                action.fail(error_e::busy, "Browser cache clearing is already pending");
                return;
            }
            // Preserve the WS result: acknowledges scheduling, with
            // eventual completion published through settings status.
            action.complete();
            return;
        }
#endif
        action.fail(error_e::unavailable, app->cef_error());
    }

    static void handle_clear_browser_cache(action_context_s& context, action_s& action)
    {
        if (!action.get_typed_payload<decltype(node_actions::clear_browser_cache)::payload_type>()) {
            action.fail(error_e::invalid_payload, "Invalid clear browser cache payload");
            return;
        }
        context.defer(clear_browser_cache);
    }

  public:
    std::string_view type() const final { return miximus::nodes::system::SETTINGS_NODE_TYPE; }

    void execute(core::app_state_s* /*app*/, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final {}

    action_dispatch_e handle_action(action_context_s& context, action_s& action) const final
    {
        if (action.name == node_actions::clear_browser_cache.name) {
            handle_clear_browser_cache(context, action);
            return action_dispatch_e::handled;
        }
        return action_dispatch_e::unhandled;
    }

    void prepare(core::app_state_s* app, const node_state_s& /* state */, prepare_result_s* /* result */) final
    {
        bool pending = false;
#if MIXIMUS_ENABLE_CEF
        if (const auto* cef = app->cef_subsystem()) {
            pending = cef->cache_clear_pending();
        }
#endif
        if (auto* registry = app->status_registry()) {
            registry->write(status_handle_,
                            status::browser_cache_status_s{.browser_cache_clearing = pending},
                            reported_cache_pending_.observe(pending) ? core::status_delivery_e::immediate
                                                                     : core::status_delivery_e::rate_limited);
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"decklink_input_buffer_frames",  decklink_input_buffer_limits_s::DEFAULT_FRAME_COUNT      },
            {"ndi_input_buffer_frames",       ndi_input_buffer_limits_s::DEFAULT_FRAME_COUNT           },
            {"cef_capture_buffer_frames",     cef_capture_buffer_limits_s::DEFAULT_FRAME_COUNT         },
            {"cef_export_buffer_frames",      cef_export_buffer_limits_s::DEFAULT_FRAME_COUNT          },
            {"cef_input_buffer_frames",       cef_input_buffer_limits_s::DEFAULT_FRAME_COUNT           },
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
        if (name == "decklink_input_buffer_frames") {
            return normalize_option_value<int>(value,
                                               decklink_input_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               decklink_input_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "ndi_input_buffer_frames") {
            return normalize_option_value<int>(
                value, ndi_input_buffer_limits_s::MINIMUM_FRAME_COUNT, ndi_input_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "cef_capture_buffer_frames") {
            return normalize_option_value<int>(value,
                                               cef_capture_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               cef_capture_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "cef_export_buffer_frames") {
            return normalize_option_value<int>(value,
                                               cef_export_buffer_limits_s::MINIMUM_FRAME_COUNT,
                                               cef_export_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
        if (name == "cef_input_buffer_frames") {
            return normalize_option_value<int>(
                value, cef_input_buffer_limits_s::MINIMUM_FRAME_COUNT, cef_input_buffer_limits_s::MAXIMUM_FRAME_COUNT);
        }
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
