#pragma once
#include "messages/transform.hpp"

namespace miximus::message {

static inline nlohmann::json create_ping_response_payload()
{
    return {
        {"action", get_action_string(action_t::ping)},
        {"response", true},
    };
}

static inline nlohmann::json create_socket_info_payload(int64_t id)
{
    return {
        {"action", get_action_string(action_t::socket_info)},
        {"id", id},
    };
}

static inline nlohmann::json create_command_base_payload(token_ref_t token)
{
    return {
        {"action", get_action_string(action_t::command)},
        {"token", token},
    };
}

static inline nlohmann::json create_result_base_payload(token_ref_t token)
{
    return {
        {"action", get_action_string(action_t::result)},
        {"token", token},
    };
}

static inline nlohmann::json create_error_base_payload(token_ref_t token, error_t error)
{
    return {
        {"action", get_action_string(action_t::error)},
        {"token", token},
        {"error", get_error_string(error)},
    };
}
} // namespace miximus::message