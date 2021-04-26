#include "web_server/web_server.hpp"
#include "web_server/detail/web_server_impl.hpp"

namespace miximus::web_server {
web_server::web_server() : impl(std::make_unique<detail::web_server_impl>()) {}

web_server::~web_server() {}

void web_server::start(std::string_view host, uint16_t port) {
  impl->start(host, port);
}

void web_server::stop() { impl->stop(); }

void web_server::broadcast_message(nlohmann::json &&msg) {
  impl->broadcast_message(std::move(msg));
}

int web_server::receive_messages(std::vector<nlohmann::json> &queue) {
  return impl->receive_messages(queue);
}

} // namespace miximus::web_server