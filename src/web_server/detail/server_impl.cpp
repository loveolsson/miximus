#include "web_server/detail/server_impl.hpp"

#include "web_server/payload_parse.hpp"

#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace miximus::web_server::detail {

web_server_impl::web_server_impl()
{
    using namespace websocketpp::log;

    endpoint_.clear_access_channels(alevel::all);
    endpoint_.set_access_channels(alevel::access_core);
    endpoint_.set_access_channels(alevel::app);
    endpoint_.set_reuse_addr(true);

    endpoint_.set_open_handler(std::bind_front(&web_server_impl::on_open, this));
    endpoint_.set_close_handler(std::bind_front(&web_server_impl::on_close, this));
    endpoint_.set_fail_handler(std::bind_front(&web_server_impl::on_fail, this));
    endpoint_.set_http_handler(std::bind_front(&web_server_impl::on_http, this));
    endpoint_.set_message_handler(std::bind_front(&web_server_impl::on_message, this));
}

void web_server_impl::subscribe(topic_e topic, const callback_t& callback)
{
    if (!started_) {
        throw std::logic_error("web_server: subscribe() called before start()");
    }
    boost::asio::post(endpoint_.get_io_context(),
                      [this, topic, callback]() { get_subscription_by_topic(topic) = callback; });
}

void web_server_impl::set_config_getters(const config_getters_t& getters)
{
    if (!started_) {
        throw std::logic_error("web_server: set_config_getters() called before start()");
    }
    boost::asio::post(endpoint_.get_io_context(), [this, getters]() { config_getters_ = getters; });
}

void web_server_impl::start(uint16_t port, boost::asio::io_context* service)
{
    using namespace websocketpp::log;

    std::error_code ec;
    endpoint_.init_asio(service, ec);
    if (ec) {
        throw std::runtime_error(std::format("web_server: init_asio failed: {}", ec.message()));
    }

    endpoint_.listen(port, ec);
    if (ec) {
        throw std::runtime_error(std::format("web_server: listen on port {} failed: {}", port, ec.message()));
    }

    endpoint_.start_accept(ec);
    if (ec) {
        // listen() succeeded (acceptor is bound), so close it immediately to
        // release the port rather than waiting for the endpoint destructor.
        websocketpp::lib::error_code ignored;
        endpoint_.stop_listening(ignored);
        throw std::runtime_error(std::format("web_server: start_accept failed: {}", ec.message()));
    }

    endpoint_.get_alog().write(alevel::app, std::format("Web server listening on port {}", port));
    started_ = true;
}

void web_server_impl::stop()
{
    if (!started_) {
        return;
    }

    using namespace websocketpp::log;
    using namespace websocketpp::close;
    using websocketpp::lib::error_code;

    endpoint_.get_alog().write(alevel::app, "Stopping server");

    std::promise<void> done;
    auto               future = done.get_future();

    boost::asio::post(endpoint_.get_io_context(), [this, p = std::move(done)]() mutable {
        endpoint_.stop_listening();

        // Suppress expected teardown noise: canceled async ops on connections
        // being closed. Safe after stop_listening() — no new external failures
        // can occur from this point.
        endpoint_.clear_access_channels(alevel::fail);

        for (auto& connection : connections_) {
            error_code ec;
            endpoint_.close(connection.first, status::going_away, "server shutting down", ec);
            if (ec) {
                endpoint_.get_alog().write(elevel::rerror, std::format("Error closing connection: {}", ec.message()));
            }
        }

        if (connections_.empty()) {
            p.set_value();
        } else {
            stop_promise_ = std::move(p);
        }
    });

    future.wait();
}

void web_server_impl::send_message(const nlohmann::json& msg, int64_t connection_id)
{
    boost::asio::post(endpoint_.get_io_context(), [this, serialized = msg.dump(), connection_id]() {
        send_message_sync(serialized, connection_id);
    });
}

void web_server_impl::send_message_sync(const nlohmann::json& msg, int64_t connection_id)
{
    send_message_sync(msg.dump(), connection_id);
}

void web_server_impl::send_message_sync(const std::string& msg, int64_t connection_id)
{
    auto hdl = connections_by_id_.find(connection_id);
    if (hdl == connections_by_id_.end()) {
        return;
    }

    auto con = connections_.find(hdl->second);
    if (con == connections_.end()) {
        return;
    }

    send(hdl->second, msg);
}

void web_server_impl::broadcast_message(const nlohmann::json& msg)
{
    auto topic = get_topic_from_payload(msg);
    if (topic.has_value()) {
        boost::asio::post(endpoint_.get_io_context(), [this, topic = *topic, serialized = msg.dump()]() {
            broadcast_message_sync(topic, serialized);
        });
    }
}

void web_server_impl::broadcast_message_sync(const nlohmann::json& msg)
{
    auto topic = get_topic_from_payload(msg);
    if (topic.has_value()) {
        broadcast_message_sync(*topic, msg.dump());
    }
}

void web_server_impl::broadcast_message_sync(topic_e topic, const std::string& msg)
{
    for (const auto& hdl : get_connections_by_topic(topic)) {
        send(hdl, msg);
    }
}

} // namespace miximus::web_server::detail
