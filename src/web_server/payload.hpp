#pragma once
#include "types/action.hpp"
#include "types/topic.hpp"

#include <nlohmann/json.hpp>

namespace miximus::web_server {
static inline std::optional<action_e> get_action_from_payload(const nlohmann::json& payload)
{
    auto act = payload.find("action");
    if (act == payload.cend() || !act->is_string()) {
        return {};
    }

    return action_from_string(act->get<std::string_view>());
}

static inline std::optional<topic_e> get_topic_from_payload(const nlohmann::json& payload)
{
    auto top = payload.find("topic");
    if (top == payload.cend() || !top->is_string()) {
        return {};
    }

    return topic_from_string(top->get<std::string_view>());
}

static inline std::string_view get_token_from_payload(const nlohmann::json& payload)
{
    auto token = payload.find("token");
    if (token == payload.cend() || !token->is_string()) {
        return {};
    }

    return token->get<std::string_view>();
}

} // namespace miximus::web_server