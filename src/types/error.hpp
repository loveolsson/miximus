#pragma once
#include "utils/lookup.hpp"

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
};

constexpr std::optional<error_e> error_from_string(std::string_view value) { return enum_from_string<error_e>(value); }

} // namespace miximus
