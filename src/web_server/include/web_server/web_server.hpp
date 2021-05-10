#pragma once
#include "messages/payload.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace miximus::web_server {
namespace detail {
class web_server_impl;
}

class web_server {
  std::unique_ptr<detail::web_server_impl> impl;

public:
  web_server();
  ~web_server();

  void subscribe(message::topic_t topic, message::callback_t callback);

  void start(uint16_t port);
  void stop();

  /**
   * Sync versions of calls should only be called from the callback thread
   */
  void send_message(const nlohmann::json &msg, int64_t connection_id);
  void send_message_sync(const nlohmann::json &msg, int64_t connection_id);
  void broadcast_message(const nlohmann::json &msg);
  void broadcast_message_sync(const nlohmann::json &msg);
};
} // namespace miximus::web_server