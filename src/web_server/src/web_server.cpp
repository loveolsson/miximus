#include "web_server/web_server.hpp"
#include "web_server_impl.hpp"

namespace miximus::web_server {
web_server::web_server()
    : impl(std::make_unique<detail::web_server_impl>())
{
}

web_server::~web_server() {}

void web_server::subscribe(message::topic_t topic, callback_t callback) { impl->subscribe(topic, callback); }

void web_server::start(uint16_t port) { impl->start(port); }

void web_server::stop() { impl->stop(); }

void web_server::send_message(const nlohmann::json& msg, int64_t connection_id)
{
    impl->send_message(msg, connection_id);
}

void web_server::send_message_sync(const nlohmann::json& msg, int64_t connection_id)
{
    impl->send_message_sync(msg, connection_id);
}

void web_server::broadcast_message(const nlohmann::json& msg) { impl->broadcast_message(msg); }

void web_server::broadcast_message_sync(const nlohmann::json& msg) { impl->broadcast_message_sync(msg); }

} // namespace miximus::web_server