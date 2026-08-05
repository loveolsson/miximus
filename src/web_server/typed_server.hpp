#pragma once
#include "logger/logger.hpp"
#include "server.hpp"
#include "types/web_message_json.hpp"
#include "types/web_message_request.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace miximus::web_server {

template <typename Message, typename Callback>
void server_s::subscribe(topic_e topic, Callback&& callback)
{
    if constexpr (requires { Message::topic; }) {
        if (topic != Message::topic) {
            throw std::logic_error("Typed web-server subscription topic does not match its message type");
        }
    }

    subscribe(topic,
              callback_t{[this, topic, callback = std::forward<Callback>(callback)](nlohmann::json&& payload,
                                                                                    int64_t connection_id) mutable {
                  const auto token = payload.find("token");
                  const auto token_value =
                      token != payload.cend() && token->is_string() ? token->get<std::string>() : std::string{};

                  Message message;
                  try {
                      payload.get_to(message);
                  } catch (const std::exception& error) {
                      if (const auto log = getlog("http"); log != nullptr) {
                          log->warn("Received malformed {} payload: {}", enum_to_string(topic), error.what());
                      }
                      send_message_sync(
                          web_message::error_s{
                              .token = token_value,
                              .error = error_e::malformed_payload,
                          },
                          connection_id);
                      return;
                  }

                  try {
                      std::invoke(callback, message, connection_id);
                  } catch (const std::exception& error) {
                      if (const auto log = getlog("http"); log != nullptr) {
                          log->error("Failed to handle {} payload: {}", enum_to_string(topic), error.what());
                      }
                      send_message_sync(
                          web_message::error_s{
                              .token = token_value,
                              .error = error_e::internal_error,
                          },
                          connection_id);
                  }
              }});
}

} // namespace miximus::web_server
