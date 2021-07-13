#include "web_server/server.hpp"
#include "server_impl.hpp"

namespace miximus::web_server {
server::server()
    : impl(std::make_unique<detail::web_server_impl>())
{
}

server::~server() { impl.reset(); }

void server::subscribe(topic_e topic, const callback_t& callback) { impl->subscribe(topic, callback); }

void server::start(uint16_t port) { impl->start(port); }

void server::stop() { impl->stop(); }

void server::send_message(const nlohmann::json& msg, int64_t connection_id) { impl->send_message(msg, connection_id); }

void server::send_message_sync(const nlohmann::json& msg, int64_t connection_id)
{
    impl->send_message_sync(msg, connection_id);
}

void server::broadcast_message(const nlohmann::json& msg) { impl->broadcast_message(msg); }

void server::broadcast_message_sync(const nlohmann::json& msg) { impl->broadcast_message_sync(msg); }

} // namespace miximus::web_server