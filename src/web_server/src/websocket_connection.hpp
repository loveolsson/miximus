#pragma once
#include "messages/message.hpp"

#include <set>
#include <string>

namespace miximus::web_server::detail {
struct websocket_connection {
  int64_t id;
  std::set<message::topic_t> topics;
};

} // namespace miximus::web_server::detail