#pragma once
#include <string>
#include <string_view>

namespace miximus::message {
typedef std::string      token_t;
typedef std::string_view token_ref_t;

enum class action_e
{
    invalid   = -1,
    subscribe = 0,
    unsubscribe,
    ping,
    socket_info,
    command,
    result,
    error,
    _count,
};

enum class topic_e
{
    invalid  = -1,
    add_node = 0,
    remove_node,
    add_connection,
    remove_connection,
    update_node,
    config,
    _count,
};

enum class error_e
{
    no_error       = -1,
    internal_error = 0,
    malformed_payload,
    invalid_topic,
    invalid_type,
    duplicate_id,
    invalid_options,
    not_found,
    _count,
};

} // namespace miximus::message