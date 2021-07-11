#pragma once
#include "messages/transform.hpp"
#include "messages/types.hpp"

#include <nlohmann/json.hpp>

namespace miximus::message {

static inline nlohmann::json create_ping_response_payload()
{
    return {
        {"action", get_action_string(action_e::ping)},
        {"response", true},
    };
}

static inline nlohmann::json create_socket_info_payload(int64_t id)
{
    return {
        {"action", get_action_string(action_e::socket_info)},
        {"id", id},
    };
}

static inline nlohmann::json create_command_base_payload(topic_e topic)
{
    return {
        {"action", get_action_string(action_e::command)},
        {"topic", get_topic_string(topic)},
    };
}

static inline nlohmann::json create_result_base_payload(token_ref_t token)
{
    return {
        {"action", get_action_string(action_e::result)},
        {"token", token},
    };
}

static inline nlohmann::json create_error_base_payload(token_ref_t token, error_e error)
{
    return {
        {"action", get_action_string(action_e::error)},
        {"token", token},
        {"error", get_error_string(error)},
    };
}
} // namespace miximus::message