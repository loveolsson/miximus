#pragma once
#include <frozen/map.h>
#include <string_view>

namespace miximus {

enum class error_e
{
    invalid  = -1,
    no_error = 0,
    internal_error,
    malformed_payload,
    invalid_topic,
    invalid_type,
    duplicate_id,
    invalid_options,
    not_found,
    circular_connection,
    _count,
};

constexpr frozen::map<std::string_view, error_e, (size_t)error_e::_count> error_lookup_table = {
    {"no_error", error_e::no_error},
    {"internal_error", error_e::internal_error},
    {"malformed_payload", error_e::malformed_payload},
    {"invalid_topic", error_e::invalid_topic},
    {"invalid_type", error_e::invalid_type},
    {"duplicate_id", error_e::duplicate_id},
    {"invalid_options", error_e::invalid_options},
    {"not_found", error_e::not_found},
    {"circular_connection", error_e::circular_connection},
};

constexpr frozen::map<error_e, std::string_view, (size_t)error_e::_count> error_resolve_table = {
    {error_e::no_error, "no_error"},
    {error_e::internal_error, "internal_error"},
    {error_e::malformed_payload, "malformed_payload"},
    {error_e::invalid_topic, "invalid_topic"},
    {error_e::invalid_type, "invalid_type"},
    {error_e::duplicate_id, "duplicate_id"},
    {error_e::invalid_options, "invalid_options"},
    {error_e::not_found, "not_found"},
    {error_e::circular_connection, "circular_connection"},
};

constexpr error_e error_from_string(std::string_view topic)
{
    auto it = error_lookup_table.find(topic);
    if (it == error_lookup_table.end()) {
        return error_e::invalid;
    }

    return it->second;
}

constexpr std::string_view error_to_string(error_e topic)
{
    auto it = error_resolve_table.find(topic);
    if (it == error_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

} // namespace miximus