#pragma once
namespace miximus {

enum class error_e
{
    no_error,
    internal_error,
    malformed_payload,
    invalid_topic,
    invalid_type,
    duplicate_id,
    invalid_options,
    not_found,
    circular_connection,
    unsupported_action,
    invalid_payload,
    busy,
    unavailable,
    cancelled,
    expired,
};

} // namespace miximus
