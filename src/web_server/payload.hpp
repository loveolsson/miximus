#pragma once
#include "types/action.hpp"
#include "types/topic.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string_view>

namespace miximus::web_server {

std::optional<action_e> get_action_from_payload(const nlohmann::json& payload);
std::optional<topic_e>  get_topic_from_payload(const nlohmann::json& payload);
std::string_view        get_token_from_payload(const nlohmann::json& payload);

} // namespace miximus::web_server
