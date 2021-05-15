#pragma once
#include "messages/transform.hpp"

#include <nlohmann/json.hpp>

namespace miximus::message {
static inline action_t get_action_from_payload(const nlohmann::json& payload)
{
    auto act = payload.find("action");
    if (act == payload.cend() || !act->is_string()) {
        return action_t::invalid;
    }

    return action_from_string(act->get<std::string_view>());
}

static inline topic_t get_topic_from_payload(const nlohmann::json& payload)
{
    auto top = payload.find("topic");
    if (top == payload.cend() || !top->is_string()) {
        return topic_t::invalid;
    }

    return topic_from_string(top->get<std::string_view>());
}

static inline token_ref_t get_token_from_payload(const nlohmann::json& payload)
{
    auto token = payload.find("token");
    if (token == payload.cend() || !token->is_string()) {
        return {};
    }

    return token->get<token_ref_t>();
}

} // namespace miximus::message