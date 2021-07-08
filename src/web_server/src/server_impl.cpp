#include "server_impl.hpp"
#include "messages/templates.hpp"
#include "utils/bind.hpp"

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
    endpoint_.clear_access_channels(websocketpp::log::alevel::all);
    endpoint_.set_access_channels(websocketpp::log::alevel::access_core);
    endpoint_.set_access_channels(websocketpp::log::alevel::app);
    endpoint_.set_reuse_addr(true);

    // Initialize the Asio transport policy
    endpoint_.init_asio();

    // Bind the handlers we are using
    endpoint_.set_open_handler(utils::bind(&web_server_impl::on_open, this));
    endpoint_.set_close_handler(utils::bind(&web_server_impl::on_close, this));
    endpoint_.set_http_handler(utils::bind(&web_server_impl::on_http, this));
    endpoint_.set_message_handler(utils::bind(&web_server_impl::on_message, this));
}

web_server_impl::~web_server_impl() { stop(); }

void web_server_impl::terminate_and_log(connection_hdl hdl, const std::string& msg)
{
    std::error_code ec;
    endpoint_.get_alog().write(websocketpp::log::alevel::fail, msg);
    endpoint_.close(std::move(hdl), websocketpp::close::status::protocol_error, msg, ec);
    if (ec) {
        endpoint_.get_alog().write(websocketpp::log::alevel::fail, ec.message());
    }
}

void web_server_impl::on_http(const connection_hdl& hdl)
{
    // Upgrade our connection handle to a full connection_ptr
    server::connection_ptr con = endpoint_.get_con_from_hdl(hdl);

    std::string_view resource = con->get_resource();

    endpoint_.get_alog().write(websocketpp::log::alevel::http, std::string(resource));

    if (resource == "/") {
        resource = "/index.html";
    }

    auto file_it = files_.find(resource.substr(1));

    if (file_it == files_.end()) {
        con->set_body(create_404_doc(resource));
        con->set_status(websocketpp::http::status_code::not_found);
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
    con->set_status(websocketpp::http::status_code::ok);
}

void web_server_impl::on_message(const connection_hdl& hdl, const message_ptr& msg)
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
    auto        doc     = nlohmann::json::parse(payload, nullptr, false);

    if (doc.is_discarded()) {
        return terminate_and_log(hdl, "invalid JSON payload");
    }

    auto action = message::get_action_from_payload(doc);

    switch (action) {
        case message::action_e::invalid: {
            terminate_and_log(hdl, "invalid action");
        } break;

        case message::action_e::ping: {
            auto pong_payload = message::create_ping_response_payload().dump();
            send(hdl, pong_payload);
        } break;

        case message::action_e::subscribe: {
            nlohmann::json response;

            auto topic = message::get_topic_from_payload(doc);
            auto token = message::get_token_from_payload(doc);

            if (topic != message::topic_e::invalid) {
                con->second.topics.emplace(topic);
                connections_by_topic_[static_cast<int>(topic)].emplace(hdl);
                response = message::create_result_base_payload(token);
            } else {
                response = message::create_error_base_payload(token, message::error_e::invalid_topic);
            }

            send(hdl, response);
        } break;

        case message::action_e::unsubscribe: {
            auto topic = message::get_topic_from_payload(doc);
            auto token = message::get_token_from_payload(doc);

            if (topic == message::topic_e::invalid) {
                send(hdl, message::create_error_base_payload(token, message::error_e::invalid_topic));
                break;
            }

            con->second.topics.erase(topic);
            connections_by_topic_[static_cast<int>(topic)].erase(hdl);
            send(hdl, message::create_result_base_payload(token));
        } break;

        case message::action_e::command: {
            auto err   = message::error_e::no_error;
            auto topic = message::get_topic_from_payload(doc);

            if (topic == message::topic_e::invalid) {
                err = message::error_e::invalid_topic;
            } else if (!subscriptions_[static_cast<int>(topic)]) {
                err = message::error_e::internal_error;
            }

            if (err != message::error_e::no_error) {
                auto token = message::get_token_from_payload(doc);
                send(hdl, message::create_error_base_payload(token, err));
                break;
            }

            auto respose_fn = [this, hdl](nlohmann::json&& payload) { send(hdl, payload); };

            subscriptions_[static_cast<int>(topic)](std::move(doc), con->second.id, respose_fn);
        } break;

        default: {
            terminate_and_log(hdl, "unhandled action");
        } break;
    }
}

void web_server_impl::on_open(const connection_hdl& hdl)
{
    endpoint_.get_alog().write(websocketpp::log::alevel::http, "Connection opened");

    auto id = next_connection_id++;

    connections_.emplace(hdl, websocket_connection{id, {}});
    connections_by_id_.emplace(id, hdl);

    send(hdl, message::create_socket_info_payload(id));
}

void web_server_impl::on_close(const connection_hdl& hdl)
{
    endpoint_.get_alog().write(websocketpp::log::alevel::http, "Connection closed");
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

void web_server_impl::send(const connection_hdl& hdl, const std::string& msg)
{
    using namespace websocketpp::frame;
    endpoint_.send(hdl, msg, opcode::text);
}

void web_server_impl::send(const connection_hdl& hdl, const nlohmann::json& msg) { send(hdl, msg.dump()); }

void web_server_impl::subscribe(message::topic_e topic, const callback_t& callback)
{
    endpoint_.get_io_service().post([this, topic, callback]() { subscriptions_[static_cast<int>(topic)] = callback; });
}

void web_server_impl::start(uint16_t port)
{
    endpoint_.get_alog().write(websocketpp::log::alevel::app, fmt::format("Starting web server on port {}", port));
    endpoint_.listen(port);
    endpoint_.start_accept();

    run_thread_ = std::make_unique<std::thread>(&server::run, &endpoint_);
}

void web_server_impl::stop()
{
    if (!run_thread_) {
        return;
    }

    endpoint_.get_alog().write(websocketpp::log::alevel::app, "Stopping server");

    endpoint_.get_io_service().post([&]() {
        endpoint_.stop_listening();

        for (auto& connection : connections_) {
            websocketpp::lib::error_code ec;
            endpoint_.close(connection.first, websocketpp::close::status::going_away, "server shutting down", ec);
            if (ec) {
                endpoint_.get_alog().write(websocketpp::log::elevel::rerror,
                                           fmt::format("Error closing connection: {}", ec.message()));
            }
        }
    });

    if (run_thread_ && run_thread_->joinable()) {
        run_thread_->join();
        run_thread_.reset();
    }
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
    auto topic      = message::get_topic_from_payload(msg);
    auto serialized = msg.dump();
    endpoint_.get_io_service().post(
        [this, topic, serialized = std::move(serialized)]() { broadcast_message_sync(topic, serialized); });
}

void web_server_impl::broadcast_message_sync(const nlohmann::json& msg)
{
    auto topic      = message::get_topic_from_payload(msg);
    auto serialized = msg.dump();
    broadcast_message_sync(topic, serialized);
}

void web_server_impl::broadcast_message_sync(message::topic_e topic, const std::string& msg)
{
    if (topic <= message::topic_e::invalid && topic >= message::topic_e::_count) {
        return;
    }

    auto& topic_set = connections_by_topic_[static_cast<int>(topic)];
    for (const auto& hdl : topic_set) {
        send(hdl, msg);
    }
}

} // namespace miximus::web_server::detail