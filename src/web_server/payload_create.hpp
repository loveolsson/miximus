#pragma once
#include "types/action.hpp"
#include "types/error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

namespace miximus::web_server {

inline nlohmann::json create_ping_response_payload()
{
    return {
        {"action",   enum_to_string(action_e::ping)},
        {"response", true                          },
    };
}

inline nlohmann::json create_socket_info_payload(int64_t id, std::string_view bundle_hash)
{
    return {
        {"action",      enum_to_string(action_e::socket_info)},
        {"id",          id                                   },
        {"bundle_hash", bundle_hash                          },
    };
}

inline nlohmann::json create_result_payload(std::string_view token)
{
    return {
        {"action", enum_to_string(action_e::result)},
        {"token",  token                           },
    };
}

inline nlohmann::json create_config_result_payload(std::string_view token, nlohmann::json config)
{
    auto payload      = create_result_payload(token);
    payload["config"] = std::move(config);
    return payload;
}

inline nlohmann::json
create_node_status_result_payload(std::string_view token, std::string_view id, nlohmann::json status)
{
    auto payload      = create_result_payload(token);
    payload["id"]     = id;
    payload["status"] = std::move(status);
    return payload;
}

inline nlohmann::json create_error_payload(std::string_view token, error_e error)
{
    return {
        {"action", enum_to_string(action_e::error)},
        {"token",  token                          },
        {"error",  enum_to_string(error)          },
    };
}

inline nlohmann::json create_error_payload(std::string_view token, error_e error, std::string_view message)
{
    auto payload       = create_error_payload(token, error);
    payload["message"] = message;
    return payload;
}

} // namespace miximus::web_server
