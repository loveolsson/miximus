#pragma once
#include "json_contract.hpp"
#include "web_message.hpp"

#include <nlohmann/json.hpp>

#include <type_traits>

namespace miximus::web_message {

template <described_json_object T>
void to_json(nlohmann::json& json, const T& message)
{
    detail::write_described_json(json, message, true);

    if constexpr (requires { typename std::integral_constant<action_e, T::action>; }) {
        json["action"] = T::action;
    }
    if constexpr (requires { typename std::integral_constant<topic_e, T::topic>; }) {
        json["topic"] = T::topic;
    }
}

template <described_json_object T>
void from_json(const nlohmann::json& json, T& message)
{
    detail::read_described_json(json, message);
}

} // namespace miximus::web_message
