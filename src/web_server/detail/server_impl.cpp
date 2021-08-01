#include "web_server/detail/server_impl.hpp"
#include "utils/bind.hpp"
#include "web_server/templates.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace miximus::web_server::detail {

static int64_t next_connection_id = 0;

static std::string create_404_doc(std::string_view resource)
{
    std::stringstream ss;

    ss << "<!doctype html><html><head>"
          "<title>Error 404 (Resource not found)</title><body>"
          "<h1>Error 404</h1>"
          "<p>The requested URL "
       << resource
       << " was not found on this server.</p>"
          "</body></head></html>";

    return ss.str();
}

web_server_impl::web_server_impl()
    : files_(static_files::get_web_files())
{
    using namespace websocketpp::log;

    endpoint_.clear_access_channels(alevel::all);
    endpoint_.set_access_channels(alevel::access_core);
    endpoint_.set_access_channels(alevel::app);
    endpoint_.set_reuse_addr(true);

    // Bind the handlers we are using
    endpoint_.set_open_handler(utils::bind(&web_server_impl::on_open, this));
    endpoint_.set_close_handler(utils::bind(&web_server_impl::on_close, this));
    endpoint_.set_http_handler(utils::bind(&web_server_impl::on_http, this));
    endpoint_.set_message_handler(utils::bind(&web_server_impl::on_message, this));
}

void web_server_impl::terminate_and_log(const con_hdl_t& hdl, const std::string& msg)
{
    using namespace websocketpp::log;
    using namespace websocketpp::close;

    std::error_code ec;
    endpoint_.get_alog().write(alevel::fail, msg);
    endpoint_.close(hdl, status::protocol_error, msg, ec);
    if (ec) {
        endpoint_.get_alog().write(alevel::fail, ec.message());
    }
}

void web_server_impl::on_http(const con_hdl_t& hdl)
{
    using namespace websocketpp::http;
    using namespace websocketpp::log;

    // Upgrade our connection handle to a full connection_ptr
    auto con = endpoint_.get_con_from_hdl(hdl);
    if (!con) {
        return;
    }

    std::string_view resource = con->get_resource();

    endpoint_.get_alog().write(alevel::http, std::string(resource));

    if (resource == "/") {
        resource = "/index.html";
    }

    auto file_it = files_.find(resource.substr(1));

    if (file_it == files_.end()) {
        con->set_body(create_404_doc(resource));
        con->set_status(status_code::not_found);
        return;
    }

    const auto& encoding = con->get_request_header("Accept-Encoding");

    if (encoding.find("gzip") != std::string::npos) {
        con->replace_header("Content-Encoding", "gzip");
        con->set_body(std::string(file_it->second.gzipped));
    } else {
        con->set_body(file_it->second.raw);
    }

    con->replace_header("Content-Type", std::string(file_it->second.mime));
    con->set_status(status_code::ok);
}

void web_server_impl::on_message(const con_hdl_t& hdl, const msg_ptr_t& msg)
{
    using namespace websocketpp::frame;

    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        return terminate_and_log(hdl, "connection not found");
    }

    if (msg->get_opcode() != opcode::text) {
        return terminate_and_log(hdl, "only text paylods are accepted");
    }

    const auto& payload = msg->get_payload();

    auto doc = nlohmann::json::parse(payload, nullptr, false);
    if (doc.is_discarded()) {
        return terminate_and_log(hdl, "invalid JSON payload");
    }

    auto action = get_action_from_payload(doc);

    switch (action) {
        case action_e::invalid: {
            terminate_and_log(hdl, "invalid action");
        } break;

        case action_e::ping: {
            auto pong_payload = create_ping_response_payload().dump();
            send(hdl, pong_payload);
        } break;

        case action_e::subscribe: {
            nlohmann::json response;

            auto topic = get_topic_from_payload(doc);
            auto token = get_token_from_payload(doc);

            if (topic != topic_e::invalid) {
                con->second.topics.emplace(topic);
                connections_by_topic_[static_cast<int>(topic)].emplace(hdl);
                response = create_result_base_payload(token);
            } else {
                response = create_error_base_payload(token, error_e::invalid_topic);
            }

            send(hdl, response);
        } break;

        case action_e::unsubscribe: {
            auto topic = get_topic_from_payload(doc);
            auto token = get_token_from_payload(doc);

            if (topic == topic_e::invalid) {
                send(hdl, create_error_base_payload(token, error_e::invalid_topic));
                break;
            }

            con->second.topics.erase(topic);
            connections_by_topic_[static_cast<int>(topic)].erase(hdl);
            send(hdl, create_result_base_payload(token));
        } break;

        case action_e::command: {
            auto err   = error_e::no_error;
            auto topic = get_topic_from_payload(doc);

            if (topic == topic_e::invalid) {
                err = error_e::invalid_topic;
            } else if (!subscription_by_topic_[static_cast<int>(topic)]) {
                err = error_e::internal_error;
            }

            if (err != error_e::no_error) {
                auto token = get_token_from_payload(doc);
                send(hdl, create_error_base_payload(token, err));
                break;
            }

            subscription_by_topic_[static_cast<int>(topic)](std::move(doc), con->second.id);
        } break;

        default: {
            terminate_and_log(hdl, "unhandled action");
        } break;
    }
}

void web_server_impl::on_open(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    endpoint_.get_alog().write(alevel::http, "Connection opened");

    auto id = next_connection_id++;

    connections_.emplace(hdl, websocket_connection{id, {}});
    connections_by_id_.emplace(id, hdl);

    send(hdl, create_socket_info_payload(id));
}

void web_server_impl::on_close(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    endpoint_.get_alog().write(alevel::http, "Connection closed");
    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        return;
    }

    auto& topics = con->second.topics;
    for (auto topic : topics) {
        connections_by_topic_[static_cast<int>(topic)].erase(hdl);
    }

    connections_by_id_.erase(con->second.id);
    connections_.erase(con);
}

void web_server_impl::send(const con_hdl_t& hdl, const std::string& msg)
{
    using namespace websocketpp::frame;
    endpoint_.send(hdl, msg, opcode::text);
}

void web_server_impl::send(const con_hdl_t& hdl, const nlohmann::json& msg) { send(hdl, msg.dump()); }

void web_server_impl::subscribe(topic_e topic, const callback_t& callback)
{
    endpoint_.get_io_service().post(
        [this, topic, callback]() { subscription_by_topic_[static_cast<int>(topic)] = callback; });
}

void web_server_impl::start(uint16_t port, boost::asio::io_service* service)
{
    using namespace websocketpp::log;
    // Initialize the Asio transport policy
    std::error_code ec;
    endpoint_.init_asio(service, ec);
    if (ec) {
        endpoint_.get_alog().write(alevel::fail, ec.message());
    }

    endpoint_.get_alog().write(alevel::app, fmt::format("Starting web server on port {}", port));
    endpoint_.listen(port);
    endpoint_.start_accept();
}

void web_server_impl::stop()
{
    using namespace websocketpp::log;
    using namespace websocketpp::close;
    using websocketpp::lib::error_code;

    endpoint_.get_alog().write(alevel::app, "Stopping server");

    endpoint_.get_io_service().post([&]() {
        endpoint_.stop_listening();

        for (auto& connection : connections_) {
            error_code ec;
            endpoint_.close(connection.first, status::going_away, "server shutting down", ec);
            if (ec) {
                endpoint_.get_alog().write(elevel::rerror, fmt::format("Error closing connection: {}", ec.message()));
            }
        }
    });
}

void web_server_impl::send_message(const nlohmann::json& msg, int64_t connection_id)
{
    auto serialized = msg.dump();
    endpoint_.get_io_service().post(
        [this, serialized = std::move(serialized), connection_id]() { send_message_sync(serialized, connection_id); });
}

void web_server_impl::send_message_sync(const nlohmann::json& msg, int64_t connection_id)
{
    auto serialized = msg.dump();
    send_message_sync(serialized, connection_id);
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
    auto topic      = get_topic_from_payload(msg);
    auto serialized = msg.dump();
    endpoint_.get_io_service().post(
        [this, topic, serialized = std::move(serialized)]() { broadcast_message_sync(topic, serialized); });
}

void web_server_impl::broadcast_message_sync(const nlohmann::json& msg)
{
    auto topic      = get_topic_from_payload(msg);
    auto serialized = msg.dump();
    broadcast_message_sync(topic, serialized);
}

void web_server_impl::broadcast_message_sync(topic_e topic, const std::string& msg)
{
    if (topic <= topic_e::invalid && topic >= topic_e::_count) {
        return;
    }

    auto& con_set = connections_by_topic_[static_cast<int>(topic)];
    for (const auto& hdl : con_set) {
        send(hdl, msg);
    }
}

} // namespace miximus::web_server::detail