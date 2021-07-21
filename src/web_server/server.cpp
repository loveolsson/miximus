#include "web_server/server.hpp"
#include "server_impl.hpp"

namespace miximus::web_server {
server_s::server_s()
    : impl(std::make_unique<detail::web_server_impl>())
{
}

server_s::~server_s() { impl.reset(); }

void server_s::subscribe(topic_e topic, const callback_t& callback) { impl->subscribe(topic, callback); }

void server_s::start(uint16_t port, boost::asio::io_service& service) { impl->start(port, service); }

void server_s::stop() { impl->stop(); }

void server_s::send_message(const nlohmann::json& msg, int64_t connection_id)
{
    impl->send_message(msg, connection_id);
}

void server_s::send_message_sync(const nlohmann::json& msg, int64_t connection_id)
{
    impl->send_message_sync(msg, connection_id);
}

void server_s::broadcast_message(const nlohmann::json& msg) { impl->broadcast_message(msg); }

void server_s::broadcast_message_sync(const nlohmann::json& msg) { impl->broadcast_message_sync(msg); }

} // namespace miximus::web_server