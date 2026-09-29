#pragma once
#include "types/action.hpp"
#include "types/topic.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace miximus::web_server {

// Count open objects/arrays, including the envelope object. Reject during parsing,
// before typed decoding can recursively copy a deeply nested document.
[[nodiscard]] inline nlohmann::json parse_websocket_payload(std::string_view payload)
{
    struct nesting_limit_s
    {
    };
    try {
        return nlohmann::json::parse(
            payload,
            [](int depth, nlohmann::json::parse_event_t event, nlohmann::json& /* value */) {
                using event_t = nlohmann::json::parse_event_t;
                if ((event == event_t::object_start || event == event_t::array_start) && depth >= 16) {
                    // Returning false would filter the value instead of rejecting it.
                    throw nesting_limit_s{};
                }
                return true;
            },
            false);
    } catch (const nesting_limit_s&) {
        // Callback exceptions propagate even with allow_exceptions=false.
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
}

[[nodiscard]] inline std::optional<action_e> get_action_from_payload(const nlohmann::json& payload)
{
    auto act = payload.find("action");
    if (act == payload.cend() || !act->is_string()) {
        return {};
    }

    return action_from_string(act->get<std::string_view>());
}

[[nodiscard]] inline std::optional<topic_e> get_topic_from_payload(const nlohmann::json& payload)
{
    auto top = payload.find("topic");
    if (top == payload.cend() || !top->is_string()) {
        return {};
    }

    return topic_from_string(top->get<std::string_view>());
}

[[nodiscard]] inline std::string_view get_token_from_payload(const nlohmann::json& payload)
{
    auto token = payload.find("token");
    if (token == payload.cend() || !token->is_string()) {
        return {};
    }

    return token->get<std::string_view>();
}

} // namespace miximus::web_server
