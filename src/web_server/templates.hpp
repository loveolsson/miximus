#pragma once
#include "types/action.hpp"
#include "types/error.hpp"
#include "types/topic.hpp"

#include <nlohmann/json.hpp>

namespace miximus::web_server {

static inline nlohmann::json create_ping_response_payload()
{
    return {
        {"action", action_to_string(action_e::ping)},
        {"response", true},
    };
}

static inline nlohmann::json create_socket_info_payload(int64_t id)
{
    return {
        {"action", action_to_string(action_e::socket_info)},
        {"id", id},
    };
}

static inline nlohmann::json create_command_base_payload(topic_e topic)
{
    return {
        {"action", action_to_string(action_e::command)},
        {"topic", topic_to_string(topic)},
    };
}

static inline nlohmann::json create_result_base_payload(std::string_view token)
{
    return {
        {"action", action_to_string(action_e::result)},
        {"token", token},
    };
}

static inline nlohmann::json create_error_base_payload(std::string_view token, error_e error)
{
    return {
        {"action", action_to_string(action_e::error)},
        {"token", token},
        {"error", error_to_string(error)},
    };
}
} // namespace miximus::web_server