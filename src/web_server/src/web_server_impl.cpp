#include "web_server_impl.hpp"
#include "messages/templates.hpp"
#include "static_files/files.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace miximus::web_server::detail {

static const auto pong_payload       = message::create_ping_response_payload().dump();
static int64_t    next_connection_id = 0;

web_server_impl::web_server_impl()
{
    endpoint_.clear_access_channels(websocketpp::log::alevel::all);
    endpoint_.set_access_channels(websocketpp::log::alevel::access_core);
    endpoint_.set_access_channels(websocketpp::log::alevel::app);

    // Initialize the Asio transport policy
    endpoint_.init_asio();

    // Bind the handlers we are using
    using websocketpp::lib::bind;
    using websocketpp::lib::placeholders::_1;
    using websocketpp::lib::placeholders::_2;
    endpoint_.set_open_handler(bind(&web_server_impl::on_open, this, _1));
    endpoint_.set_close_handler(bind(&web_server_impl::on_close, this, _1));
    endpoint_.set_http_handler(bind(&web_server_impl::on_http, this, _1));
    endpoint_.set_message_handler(bind(&web_server_impl::on_message, this, _1, _2));
}

web_server_impl::~web_server_impl() { stop(); }

void web_server_impl::terminate_and_log(connection_hdl hdl, const std::string& msg)
{
    std::error_code ec;
    endpoint_.get_alog().write(websocketpp::log::alevel::app, msg);
    endpoint_.close(hdl, websocketpp::close::status::protocol_error, msg, ec);
    if (ec) {
        endpoint_.get_alog().write(websocketpp::log::alevel::fail, ec.message());
    }
}

void web_server_impl::on_http(connection_hdl hdl)
{
    // Upgrade our connection handle to a full connection_ptr
    server::connection_ptr con = endpoint_.get_con_from_hdl(hdl);

    std::string_view resource = con->get_resource();

    endpoint_.get_alog().write(websocketpp::log::alevel::app, std::string(resource));

    if (resource == "/") {
        resource = "/index.html";
    }

    auto& files   = static_files::get_web_files();
    auto  file_it = files.find(resource.substr(1));

    if (file_it != files.end()) {
        auto&            encoding = con->get_request_header("Accept-Encoding");
        std::string_view data;
        if (encoding.find("gzip") != std::string::npos) {
            con->replace_header("Content-Encoding", "gzip");
            con->set_body(file_it->second.gzipped);
        } else {
            con->set_body(file_it->second.raw);
        }

        con->replace_header("Content-Type", file_it->second.mime);
        con->set_status(websocketpp::http::status_code::ok);
        return;
    }

    // 404 error
    std::stringstream ss;

    ss << "<!doctype html><html><head>"
          "<title>Error 404 (Resource not found)</title><body>"
          "<h1>Error 404</h1>"
          "<p>The requested URL "
       << resource
       << " was not found on this server.</p>"
          "</body></head></html>";

    con->set_body(ss.str());
    con->set_status(websocketpp::http::status_code::not_found);
}

void web_server_impl::on_message(connection_hdl hdl, message_ptr msg)
{
    using namespace websocketpp::frame;
    using websocketpp::log::alevel;

    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        return terminate_and_log(hdl, "WebSocket connection not found");
    }

    if (msg->get_opcode() != opcode::text) {
        return terminate_and_log(hdl, "Error: Only text paylods are accepted");
    }

    auto& payload = msg->get_payload();
    auto  doc     = nlohmann::json::parse(payload, nullptr, false);

    if (doc.is_discarded()) {
        return terminate_and_log(hdl, "Error: Invalid JSON payload");
    }

    auto action = message::get_action_from_payload(doc);

    switch (action) {
        case message::action_t::invalid: {
            terminate_and_log(hdl, "Error: Invalid action");
        } break;

        case message::action_t::ping: {
            endpoint_.send(hdl, pong_payload, opcode::text);
        } break;

        case message::action_t::subscribe: {
            nlohmann::json response;
            auto           topic = message::get_topic_from_payload(doc);
            auto           token = message::get_token_from_payload(doc);

            if (topic != message::topic_t::invalid) {
                con->second.topics.emplace(topic);
                connections_by_topic_[(int)topic].emplace(hdl);
                response = message::create_result_base_payload(token);
            } else {
                response = message::create_error_base_payload(token, message::error_t::invalid_topic);
            }
            endpoint_.send(hdl, response.dump(), opcode::text);
        } break;

        case message::action_t::unsubscribe: {
            nlohmann::json response;
            auto           topic = message::get_topic_from_payload(doc);
            auto           token = message::get_token_from_payload(doc);

            if (topic != message::topic_t::invalid) {
                con->second.topics.erase(topic);
                connections_by_topic_[(int)topic].erase(hdl);
                response = message::create_result_base_payload(token);
            } else {
                response = message::create_error_base_payload(token, message::error_t::invalid_topic);
            }
            endpoint_.send(hdl, response.dump(), opcode::text);
        } break;

        case message::action_t::command: {
            auto topic = message::get_topic_from_payload(doc);
            if (topic > message ::topic_t::invalid && topic < message::topic_t::_count && subscriptions_[(int)topic]) {
                auto respose_fn = [this, hdl](nlohmann::json&& payload) {
                    endpoint_.send(hdl, payload.dump(), opcode::text);
                };

                subscriptions_[(int)topic](std::move(doc), con->second.id, respose_fn);
            } else {
                auto token = message::get_token_from_payload(doc);
                endpoint_.send(hdl,
                               message::create_error_base_payload(token, message::error_t::internal_error).dump(),
                               opcode::text);
            }
        } break;

        default: {
            terminate_and_log(hdl, "Error: Unhandled action");
        } break;
    }
}

void web_server_impl::on_open(connection_hdl hdl)
{
    endpoint_.get_alog().write(websocketpp::log::alevel::app, "Connection opened");
    auto con = connections_.emplace(hdl, websocket_connection{next_connection_id++, {}});

    auto id = con.first->second.id;
    connections_by_id_.emplace(id, hdl);

    auto socket_info = message::create_socket_info_payload(id);
    endpoint_.send(hdl, socket_info.dump(), websocketpp::frame::opcode::text);
}

void web_server_impl::on_close(connection_hdl hdl)
{
    endpoint_.get_alog().write(websocketpp::log::alevel::app, "Connection closed");
    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        return;
    }

    auto& topics = con->second.topics;
    for (auto it = topics.begin(); it != topics.end(); ++it) {
        connections_by_topic_[(int)*it].erase(hdl);
    }

    connections_by_id_.erase(con->second.id);
    connections_.erase(con);
}

void web_server_impl::subscribe(message::topic_t topic, callback_t callback)
{
    endpoint_.get_io_service().post([this, topic, callback]() { subscriptions_[(int)topic] = callback; });
}

void web_server_impl::start(uint16_t port)
{
    std::stringstream ss;
    ss << "Starting web server on port " << port;
    endpoint_.get_alog().write(websocketpp::log::alevel::app, ss.str());
    endpoint_.listen(port);
    endpoint_.start_accept();

    run_thread_ = std::make_unique<std::thread>(&server::run, &endpoint_);
}

void web_server_impl::stop()
{
    if (!run_thread_) {
        return;
    }

    std::cout << "Stopping server" << std::endl;

    endpoint_.get_io_service().post([&]() {
        endpoint_.stop_listening();

        for (auto it = connections_.begin(); it != connections_.end(); it++) {
            websocketpp::lib::error_code ec;
            endpoint_.close(it->first, websocketpp::close::status::going_away, "Server shutting down", ec);
            if (ec) {
                std::cout << "> Error closing connection: " << ec.message() << std::endl;
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

    endpoint_.send(hdl->second, msg, websocketpp::frame::opcode::text);
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

void web_server_impl::broadcast_message_sync(message::topic_t topic, const std::string& msg)
{
    if (topic <= message::topic_t::invalid && topic >= message::topic_t::_count) {
        return;
    }

    auto& topic_set = connections_by_topic_[(int)topic];

    for (auto it = topic_set.begin(); it != topic_set.end(); it++) {
        endpoint_.send(*it, msg, websocketpp::frame::opcode::text);
    }
}

} // namespace miximus::web_server::detail