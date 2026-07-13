#pragma once
#include "types/action.hpp"
#include "types/error.hpp"
#include "types/topic.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string_view>

namespace miximus::web_server {

nlohmann::json create_ping_response_payload();
nlohmann::json create_socket_info_payload(int64_t id, std::string_view bundle_hash);
nlohmann::json create_command_base_payload(topic_e topic);
nlohmann::json create_result_base_payload(std::string_view token);
nlohmann::json create_error_base_payload(std::string_view token, error_e error);

} // namespace miximus::web_server
