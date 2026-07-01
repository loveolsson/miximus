#include "web_server/detail/server_impl.hpp"
#include "headers.hpp"
#include "html.hpp"
#include "utils/lookup.hpp"
#include "web_server/templates.hpp"
#include <boost/url/parse.hpp>
#include <boost/url/url_view.hpp>

#include <exception>
#include <fmt/format.h>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::string_view API_V1_PREFIX = "/api/v1/";

constexpr std::string_view HTTP_GET     = "GET";
constexpr std::string_view HTTP_POST    = "POST";
constexpr std::string_view HTTP_OPTIONS = "OPTIONS";

} // namespace

namespace miximus::web_server::detail {

web_server_impl::web_server_impl()
{
    using namespace websocketpp::log;

    endpoint_.clear_access_channels(alevel::all);
    endpoint_.set_access_channels(alevel::access_core);
    endpoint_.set_access_channels(alevel::app);
    endpoint_.set_reuse_addr(true);

    // Bind the handlers we are using
    endpoint_.set_open_handler(std::bind_front(&web_server_impl::on_open, this));
    endpoint_.set_close_handler(std::bind_front(&web_server_impl::on_close, this));
    endpoint_.set_fail_handler(std::bind_front(&web_server_impl::on_fail, this));
    endpoint_.set_http_handler(std::bind_front(&web_server_impl::on_http, this));
    endpoint_.set_message_handler(std::bind_front(&web_server_impl::on_message, this));
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

    // Extract connection_ptr from connection handle
    websocketpp::lib::error_code ec;
    auto                         con = endpoint_.get_con_from_hdl(hdl, ec);
    if (ec) {
        return;
    }

    const auto& resource = con->get_resource();
    const auto& method   = con->get_request().get_method();

    auto             r      = boost::urls::parse_relative_ref(resource);
    auto             parsed = r.has_value() ? *r : boost::urls::url_view{};
    std::string_view path   = parsed.encoded_path();
    if (path.empty()) {
        path = "/";
    }

    endpoint_.get_alog().write(alevel::http, std::string(path));

    // Handle API routes
    if (path.starts_with(API_V1_PREFIX)) {
        handle_api_request(con, method, path.substr(API_V1_PREFIX.length()));
        return;
    }

    // Handle static file serving (original logic)
    std::string_view resource_view = path;
    if (resource_view == "/") {
        resource_view = "/index.html";
    }

    const auto& files   = static_files::get_web_files();
    const auto  file_it = files.get_file(resource_view.substr(1));

    if (file_it == nullptr) {
        con->set_body(create_404_body(std::string(path)));
        con->replace_header("Content-Type", "text/html;charset=UTF-8");
        con->set_status(status_code::not_found);
        return;
    }

    const auto etag          = '"' + std::string(file_it->etag) + '"';
    const auto if_none_match = con->get_request_header("If-None-Match");
    if (!if_none_match.empty() && if_none_match == etag) {
        con->set_status(status_code::not_modified);
        return;
    }

    if (accept_encoding_has_gzip(con->get_request_header("Accept-Encoding"))) {
        con->replace_header("Content-Encoding", "gzip");
        con->set_body(std::string(file_it->gzipped));
    } else {
        con->set_body(file_it->unzip());
    }

    con->replace_header("Content-Type", std::string(file_it->mime));
    con->replace_header("ETag", etag);
    con->replace_header("Cache-Control", "no-cache");
    con->set_status(status_code::ok);
}

void web_server_impl::handle_api_request(const server_t::connection_ptr& con,
                                         const std::string&              method,
                                         std::string_view                api_path)
{
    using namespace websocketpp::http;

    // Set CORS headers for API requests
    con->replace_header("Access-Control-Allow-Origin", "*");
    con->replace_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    con->replace_header("Access-Control-Allow-Headers", "Content-Type");
    con->replace_header("Content-Type", "application/json");

    // Handle OPTIONS request for CORS preflight
    if (method == HTTP_OPTIONS) {
        con->set_status(status_code::ok);
        con->set_body("{}");
        return;
    }

    try {
        if (method == HTTP_GET && api_path == "config") {
            handle_api_v1_get_config(con);
        } else if (method == HTTP_POST && api_path == "control") {
            handle_api_v1_post_control(con);
        } else {
            // Method or endpoint not found
            auto error       = create_error_base_payload("", error_e::internal_error);
            error["message"] = "Invalid API endpoint or method";
            con->set_body(error.dump());
            con->set_status(status_code::not_found);
        }
    } catch (const std::exception& e) {
        auto error       = create_error_base_payload("", error_e::internal_error);
        error["message"] = e.what();
        con->set_body(error.dump());
        con->set_status(status_code::internal_server_error);
    }
}

void web_server_impl::handle_api_v1_get_config(const server_t::connection_ptr& con) const
{
    using namespace websocketpp::http;

    // Get current config
    if (!config_getters_.node_config) {
        auto error       = create_error_base_payload("", error_e::internal_error);
        error["message"] = "Config service not available";
        con->set_body(error.dump());
        con->set_status(status_code::service_unavailable);
        return;
    }

    try {
        const nlohmann::json config = config_getters_.node_config();
        con->set_body(config.dump());
        con->set_status(status_code::ok);
    } catch (const std::exception& e) {
        auto error       = create_error_base_payload("", error_e::internal_error);
        error["message"] = e.what();
        con->set_body(error.dump());
        con->set_status(status_code::internal_server_error);
    }
}

void web_server_impl::handle_api_v1_post_control(const server_t::connection_ptr& con)
{
    using namespace websocketpp::http;

    // Parse POST body as WebSocket-style message
    const std::string& body = con->get_request_body();

    if (body.empty()) {
        auto error       = create_error_base_payload("", error_e::malformed_payload);
        error["message"] = "Request body is required";
        con->set_body(error.dump());
        con->set_status(status_code::bad_request);
        return;
    }

    auto doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        auto error       = create_error_base_payload("", error_e::malformed_payload);
        error["message"] = "Invalid JSON in request body";
        con->set_body(error.dump());
        con->set_status(status_code::bad_request);
        return;
    }

    auto action = get_action_from_payload(doc);
    if (!action.has_value()) {
        auto error       = create_error_base_payload("", error_e::malformed_payload);
        error["message"] = "Invalid action";
        con->set_body(error.dump());
        con->set_status(status_code::bad_request);
        return;
    }

    if (*action != action_e::command) {
        auto error       = create_error_base_payload("", error_e::malformed_payload);
        error["message"] = "Only command actions are supported via HTTP API";
        con->set_body(error.dump());
        con->set_status(status_code::bad_request);
        return;
    }

    auto topic = get_topic_from_payload(doc);

    if (!topic.has_value()) {
        auto error       = create_error_base_payload("", error_e::invalid_topic);
        error["message"] = "Invalid topic";
        con->set_body(error.dump());
        con->set_status(status_code::bad_request);
        return;
    }

    // Handle the command using the shared method
    auto error_code = handle_user_command(std::move(doc), -1); // Use connection ID -1 for HTTP

    if (error_code != error_e::no_error) {
        auto error = create_error_base_payload("", error_code);
        if (error_code == error_e::invalid_topic) {
            error["message"] = "Invalid topic";
            con->set_status(status_code::bad_request);
        } else {
            error["message"] = "Topic service not available";
            con->set_status(status_code::service_unavailable);
        }
        con->set_body(error.dump());
        return;
    }

    // Return 204 No Content - command processed, no response needed
    con->set_status(status_code::no_content);
}

error_e web_server_impl::handle_user_command(nlohmann::json&& doc, int64_t connection_id)
{
    auto topic = get_topic_from_payload(doc);

    if (!topic.has_value()) {
        return error_e::invalid_topic;
    }

    const auto& subscription = get_subscription_by_topic(*topic);

    if (!subscription) {
        return error_e::internal_error;
    }

    // Process the command through the existing subscription system
    subscription(std::move(doc), connection_id);

    return error_e::no_error; // Success
}

void web_server_impl::on_message(const con_hdl_t& hdl, const msg_ptr_t& msg)
{
    using namespace websocketpp::frame;

    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        terminate_and_log(hdl, "connection not found");
        return;
    }

    if (msg->get_opcode() != opcode::text) {
        terminate_and_log(hdl, "only text paylods are accepted");
        return;
    }

    auto doc = nlohmann::json::parse(msg->get_payload(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        terminate_and_log(hdl, "invalid JSON payload");
        return;
    }

    auto action = get_action_from_payload(doc);
    if (!action.has_value()) {
        terminate_and_log(hdl, "invalid action");
        return;
    }

    switch (*action) {
        case action_e::ping: {
            send(hdl, create_ping_response_payload());
        } break;

        case action_e::subscribe: {
            nlohmann::json response;

            auto topic = get_topic_from_payload(doc);
            auto token = get_token_from_payload(doc);

            if (topic.has_value()) {
                con->second.set_subscription(*topic, true);
                get_connections_by_topic(*topic).emplace(hdl);

                response = create_result_base_payload(token);
            } else {
                response = create_error_base_payload(token, error_e::invalid_topic);
            }

            send(hdl, response);
        } break;

        case action_e::unsubscribe: {
            auto topic = get_topic_from_payload(doc);
            auto token = get_token_from_payload(doc);

            if (!topic.has_value()) {
                send(hdl, create_error_base_payload(token, error_e::invalid_topic));
                break;
            }

            if (!con->second.has_subscription(*topic)) {
                send(hdl, create_error_base_payload(token, error_e::not_found));
                break;
            }

            con->second.set_subscription(*topic, false);
            get_connections_by_topic(*topic).erase(hdl);
            send(hdl, create_result_base_payload(token));
        } break;

        case action_e::command: {
            auto token      = get_token_from_payload(doc);
            auto error_code = handle_user_command(std::move(doc), con->second.id);

            if (error_code != error_e::no_error) {
                send(hdl, create_error_base_payload(token, error_code));
            }
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

    auto id = next_connection_id_++;

    connections_.emplace(hdl, websocket_connection{.id = id, .topics = {}});
    connections_by_id_.emplace(id, hdl);

    send(hdl, create_socket_info_payload(id));
}

void web_server_impl::on_fail(const con_hdl_t& hdl)
{
    websocketpp::lib::error_code ec;
    auto                         con = endpoint_.get_con_from_hdl(hdl, ec);
    if (ec) {
        return;
    }

    auto log = getlog("http");
    if (!log) {
        return;
    }

    const auto& conn_ec = con->get_ec();
    // When stop_listening() cancels the pending async_accept, websocketpp's
    // handle_accept converts asio::errc::operation_canceled to
    // websocketpp::error::operation_canceled.  The pre-allocated accept socket
    // (fd=-1) then emits EBADF on shutdown/remote_endpoint.  All benign.
    if (conn_ec == websocketpp::error::make_error_code(websocketpp::error::operation_canceled)) {
        log->debug("Connection canceled during accept (shutdown artifact)");
        return;
    }
    // Real failure: avoid con->get_remote_endpoint() on a never-accepted socket
    // because that call itself triggers an EBADF log via the error logger.
    log->warn("Connection failed: {} ({})", conn_ec.value(), conn_ec.message());
}

void web_server_impl::on_close(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    endpoint_.get_alog().write(alevel::http, "Connection closed");
    auto con = connections_.find(hdl);
    if (con == connections_.end()) {
        return;
    }

    for (auto t : magic_enum::enum_values<topic_e>()) {
        if (con->second.has_subscription(t)) {
            get_connections_by_topic(t).erase(hdl);
        }
    }

    connections_by_id_.erase(con->second.id);
    connections_.erase(con);

    if (connections_.empty() && stop_promise_) {
        // Post rather than set directly: we are still inside handle_terminate(),
        // which calls log_close_result() and m_termination_handler() after
        // returning from on_close().  m_termination_handler() accesses endpoint_
        // to remove the connection from its internal tracking.  By posting, we
        // guarantee those steps finish before stop() unblocks and the caller is
        // free to destroy the web_server (and endpoint_).
        auto p = std::move(*stop_promise_);
        stop_promise_.reset();
        boost::asio::post(endpoint_.get_io_context(), [p = std::move(p)]() mutable { p.set_value(); });
    }
}

void web_server_impl::send(const con_hdl_t& hdl, const std::string& msg)
{
    using namespace websocketpp::log;
    std::error_code ec;
    endpoint_.send(hdl, msg, websocketpp::frame::opcode::text, ec);
    if (ec) {
        endpoint_.get_alog().write(alevel::fail, ec.message());
    }
}

void web_server_impl::send(const con_hdl_t& hdl, const nlohmann::json& msg) { send(hdl, msg.dump()); }

void web_server_impl::subscribe(topic_e topic, const callback_t& callback)
{
    boost::asio::post(endpoint_.get_io_context(),
                      [this, topic, callback]() { get_subscription_by_topic(topic) = callback; });
}

void web_server_impl::set_config_getters(const config_getters_t& getters) { config_getters_ = getters; }

void web_server_impl::start(uint16_t port, boost::asio::io_context* service)
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
                endpoint_.get_alog().write(elevel::rerror, fmt::format("Error closing connection: {}", ec.message()));
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