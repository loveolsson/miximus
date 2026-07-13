#include "templates.hpp"

#include <nlohmann/json.hpp>

namespace miximus::web_server {

nlohmann::json create_ping_response_payload()
{
    return {
        {"action",   enum_to_string(action_e::ping)},
        {"response", true                          },
    };
}

nlohmann::json create_socket_info_payload(int64_t id, std::string_view bundle_hash)
{
    return {
        {"action",      enum_to_string(action_e::socket_info)},
        {"id",          id                                   },
        {"bundle_hash", bundle_hash                          },
    };
}

nlohmann::json create_command_base_payload(topic_e topic)
{
    return {
        {"action", enum_to_string(action_e::command)},
        {"topic",  enum_to_string(topic)            },
    };
}

nlohmann::json create_result_base_payload(std::string_view token)
{
    return {
        {"action", enum_to_string(action_e::result)},
        {"token",  token                           },
    };
}

nlohmann::json create_error_base_payload(std::string_view token, error_e error)
{
    return {
        {"action", enum_to_string(action_e::error)},
        {"token",  token                          },
        {"error",  enum_to_string(error)          },
    };
}

} // namespace miximus::web_server
