#pragma once
#include <nlohmann/json_fwd.hpp>

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

  void start(std::string_view host, uint16_t port);
  void stop();

  void broadcast_message(nlohmann::json &&msg);
  int receive_messages(std::vector<nlohmann::json> &queue);
};
} // namespace miximus::web_server