#include "logger/logger.hpp"
#include "static_files/files.hpp"
#include "web_server/detail/server_impl.hpp"
#include "web_server/payload_create.hpp"
#include "web_server/payload_parse.hpp"

#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <system_error>
#include <utility>

namespace miximus::web_server::detail {

void web_server_impl::terminate_and_log(const con_hdl_t& hdl, const std::string& message)
{
    using namespace websocketpp::log;
    using namespace websocketpp::close;

    std::error_code error;
    endpoint_.get_alog().write(alevel::fail, message);
    endpoint_.close(hdl, status::protocol_error, message, error);
    if (error) {
        endpoint_.get_alog().write(alevel::fail, error.message());
    }
}

error_e web_server_impl::handle_user_command(nlohmann::json&& doc, int64_t connection_id)
{
    const auto topic = get_topic_from_payload(doc);
    if (!topic.has_value()) {
        return error_e::invalid_topic;
    }

    const auto& subscription = get_subscription_by_topic(*topic);
    if (!subscription) {
        return error_e::internal_error;
    }

    subscription(std::move(doc), connection_id);
    return error_e::no_error;
}

void web_server_impl::on_message(const con_hdl_t& hdl, const msg_ptr_t& msg)
{
    using namespace websocketpp::frame;

    auto connection = connections_.find(hdl);
    if (connection == connections_.end()) {
        terminate_and_log(hdl, "connection not found");
        return;
    }

    if (msg->get_opcode() != opcode::text) {
        terminate_and_log(hdl, "only text payloads are accepted");
        return;
    }

    auto doc = nlohmann::json::parse(msg->get_payload(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        terminate_and_log(hdl, "invalid JSON payload");
        return;
    }

    const auto action = get_action_from_payload(doc);
    if (!action.has_value()) {
        terminate_and_log(hdl, "invalid action");
        return;
    }

    switch (*action) {
        case action_e::ping:
            send(hdl, create_ping_response_payload());
            break;

        case action_e::subscribe: {
            nlohmann::json response;
            const auto     topic = get_topic_from_payload(doc);
            const auto     token = get_token_from_payload(doc);

            if (topic.has_value()) {
                connection->second.set_subscription(*topic, true);
                get_connections_by_topic(*topic).emplace(hdl);
                response = create_result_payload(token);
            } else {
                response = create_error_payload(token, error_e::invalid_topic);
            }

            send(hdl, response);
            break;
        }

        case action_e::unsubscribe: {
            const auto topic = get_topic_from_payload(doc);
            const auto token = get_token_from_payload(doc);

            if (!topic.has_value()) {
                send(hdl, create_error_payload(token, error_e::invalid_topic));
                break;
            }
            if (!connection->second.has_subscription(*topic)) {
                send(hdl, create_error_payload(token, error_e::not_found));
                break;
            }

            connection->second.set_subscription(*topic, false);
            get_connections_by_topic(*topic).erase(hdl);
            send(hdl, create_result_payload(token));
            break;
        }

        case action_e::command: {
            const auto token      = get_token_from_payload(doc);
            const auto error_code = handle_user_command(std::move(doc), connection->second.id);
            if (error_code != error_e::no_error) {
                send(hdl, create_error_payload(token, error_code));
            }
            break;
        }

        default:
            terminate_and_log(hdl, "unhandled action");
            break;
    }
}

void web_server_impl::on_open(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    endpoint_.get_alog().write(alevel::http, "Connection opened");
    const auto id = next_connection_id_++;
    connections_.emplace(hdl, websocket_connection{.id = id, .topics = {}});
    connections_by_id_.emplace(id, hdl);
    send(hdl, create_socket_info_payload(id, static_files::get_web_files().bundle_hash));
}

void web_server_impl::on_fail(const con_hdl_t& hdl)
{
    websocketpp::lib::error_code error;
    auto                         connection = endpoint_.get_con_from_hdl(hdl, error);
    if (error) {
        return;
    }

    auto log = getlog("http");
    if (!log) {
        return;
    }

    const auto& connection_error = connection->get_ec();
    // stop_listening() cancels the pending accept during shutdown. Websocketpp
    // reports that expected cancellation through the failure handler.
    if (connection_error == websocketpp::error::make_error_code(websocketpp::error::operation_canceled)) {
        log->debug("Connection canceled during accept (shutdown artifact)");
        return;
    }

    // Avoid get_remote_endpoint() on a never-accepted socket; websocketpp logs
    // another EBADF while trying to retrieve it.
    log->warn("Connection failed: {} ({})", connection_error.value(), connection_error.message());
}

void web_server_impl::on_close(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    endpoint_.get_alog().write(alevel::http, "Connection closed");
    auto connection = connections_.find(hdl);
    if (connection == connections_.end()) {
        return;
    }

    for (const auto topic : magic_enum::enum_values<topic_e>()) {
        if (connection->second.has_subscription(topic)) {
            get_connections_by_topic(topic).erase(hdl);
        }
    }

    connections_by_id_.erase(connection->second.id);
    connections_.erase(connection);

    if (connections_.empty() && stop_promise_.has_value()) {
        // The endpoint still uses itself after invoking this handler. Resolve
        // the stop promise in the next Asio turn so destruction cannot race
        // websocketpp's termination bookkeeping.
        auto promise = std::move(*stop_promise_);
        stop_promise_.reset();
        boost::asio::post(endpoint_.get_io_context(),
                          [promise = std::move(promise)]() mutable { promise.set_value(); });
    }
}

void web_server_impl::send(const con_hdl_t& hdl, const std::string& message)
{
    using namespace websocketpp::log;

    std::error_code error;
    endpoint_.send(hdl, message, websocketpp::frame::opcode::text, error);
    if (error) {
        endpoint_.get_alog().write(alevel::fail, error.message());
    }
}

void web_server_impl::send(const con_hdl_t& hdl, const nlohmann::json& message) { send(hdl, message.dump()); }

} // namespace miximus::web_server::detail
