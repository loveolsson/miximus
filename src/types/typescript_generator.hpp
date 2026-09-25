#pragma once
#include "cef_status.hpp"
#include "decklink_status.hpp"
#include "frame_rate.hpp"
#include "gpu/types.hpp"
#include "json_contract.hpp"
#include "settings_option.hpp"
#include "utils/flicks.hpp"
#include "web_message_request.hpp"

#include <concepts>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace miximus::typescript {

template <typename>
inline constexpr bool always_false = false;

template <typename T>
struct vector_traits
{
    static constexpr bool value = false;
};

template <typename T, typename Allocator>
struct vector_traits<std::vector<T, Allocator>>
{
    static constexpr bool value = true;
    using value_type            = T;
};

template <typename T>
std::string typescript_type()
{
    if constexpr (std::same_as<T, bool>) {
        return "boolean";
    } else if constexpr (std::integral<T> || std::floating_point<T> || std::same_as<T, utils::flicks>) {
        return "number";
    } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
        return "string";
    } else if constexpr (std::same_as<T, cef_state_e>) {
        return "cef_state_e";
    } else if constexpr (std::same_as<T, cef_input_state_e>) {
        return "cef_input_state_e";
    } else if constexpr (std::same_as<T, decklink_keyer_mode_e>) {
        return "decklink_keyer_mode_e";
    } else if constexpr (std::same_as<T, action_e>) {
        return "action_e";
    } else if constexpr (std::same_as<T, topic_e>) {
        return "topic_e";
    } else if constexpr (std::same_as<T, error_e>) {
        return "error_e";
    } else if constexpr (std::same_as<T, font_registry_command_e>) {
        return "font_registry_command_e";
    } else if constexpr (miximus::detail::optional_traits<T>::value) {
        return typescript_type<typename miximus::detail::optional_traits<T>::value_type>() + " | null";
    } else if constexpr (vector_traits<T>::value) {
        return "ReadonlyArray<" + typescript_type<typename vector_traits<T>::value_type>() + ">";
    } else if constexpr (std::same_as<T, frame_rate_s>) {
        return "frame_rate_s";
    } else if constexpr (std::same_as<T, settings_option_s>) {
        return "settings_option_s";
    } else if constexpr (std::same_as<T, gpu::vec2_t>) {
        return "vec2_t";
    } else if constexpr (std::same_as<T, connection_s>) {
        return "connection_s";
    } else if constexpr (std::same_as<T, web_message::node_s>) {
        return "node_s";
    } else if constexpr (std::same_as<T, web_message::config_s>) {
        return "config_s";
    } else {
        static_assert(always_false<T>, "Unsupported TypeScript contract member type");
    }
}

// Only these specific members carry opaque JSON with a known TypeScript shape.
template <auto Member>
inline constexpr std::string_view member_type_override{};

template <>
inline constexpr std::string_view member_type_override<&web_message::node_s::options> = "options_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::add_node_request_s::options> = "options_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::update_node_command_s::options> = "options_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::update_node_request_s::options> = "options_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::node_status_result_s::status> = "node_status_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::node_status_command_s::status> = "node_status_s";
template <>
inline constexpr std::string_view member_type_override<&web_message::config_s::status> =
    "Readonly<Record<string, node_status_s>> | null";

template <>
inline constexpr std::string_view member_type_override<&web_message::node_action_request_s::payload> = "unknown";
template <>
inline constexpr std::string_view member_type_override<&web_message::node_action_result_s::data> = "unknown";

template <typename Member, auto Pointer>
std::string typescript_member_type()
{
    if constexpr (!member_type_override<Pointer>.empty()) {
        return std::string(member_type_override<Pointer>);
    } else {
        return typescript_type<Member>();
    }
}

std::string generate_typescript();
bool        write_if_changed(const std::filesystem::path& path, std::string_view contents);

} // namespace miximus::typescript
