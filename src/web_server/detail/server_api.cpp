#include "web_server/detail/server_impl.hpp"
#include "web_server/payload_create.hpp"
#include "web_server/payload_parse.hpp"

#include <boost/url/pct_string_view.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view HTTP_GET      = "GET";
constexpr std::string_view HTTP_POST     = "POST";
constexpr std::string_view HTTP_OPTIONS  = "OPTIONS";
constexpr std::string_view NODES_PREFIX  = "nodes/";
constexpr std::string_view STATUS_SUFFIX = "/status";

template <typename Connection>
void handle_keyed_json_get(const Connection&                               connection,
                           const miximus::web_server::keyed_json_getter_t& getter,
                           std::string_view                                encoded_id,
                           std::string_view                                service_name)
{
    using namespace websocketpp::http;

    if (!getter) {
        const auto error = miximus::web_server::create_error_payload(
            "", miximus::error_e::internal_error, std::format("{} service not available", service_name));
        connection->set_body(error.dump());
        connection->set_status(status_code::service_unavailable);
        return;
    }

    const auto id     = boost::urls::pct_string_view(encoded_id.data(), encoded_id.size()).decode();
    const auto result = getter(id);
    if (!result.has_value()) {
        const auto error = miximus::web_server::create_error_payload(
            "", miximus::error_e::not_found, std::format("Node {} not found", id));
        connection->set_body(error.dump());
        connection->set_status(status_code::not_found);
        return;
    }

    connection->set_body(result->dump());
    connection->set_status(status_code::ok);
}

} // namespace

namespace miximus::web_server::detail {

void web_server_impl::handle_api_request(const server_t::connection_ptr& connection,
                                         const std::string&              method,
                                         std::string_view                api_path)
{
    using namespace websocketpp::http;

    connection->replace_header("Access-Control-Allow-Origin", "*");
    connection->replace_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    connection->replace_header("Access-Control-Allow-Headers", "Content-Type");
    connection->replace_header("Content-Type", "application/json");

    if (method == HTTP_OPTIONS) {
        connection->set_status(status_code::ok);
        connection->set_body("{}");
        return;
    }

    try {
        if (method == HTTP_GET && api_path == "config") {
            handle_api_v1_get_config(connection);
        } else if (method == HTTP_GET && api_path.starts_with(NODES_PREFIX)) {
            const auto node_path = api_path.substr(NODES_PREFIX.size());
            if (node_path.ends_with(STATUS_SUFFIX)) {
                handle_api_v1_get_node_status(connection, node_path.substr(0, node_path.size() - STATUS_SUFFIX.size()));
            } else {
                handle_api_v1_get_node(connection, node_path);
            }
        } else if (method == HTTP_POST && api_path == "control") {
            handle_api_v1_post_control(connection);
        } else {
            const auto error = create_error_payload("", error_e::internal_error, "Invalid API endpoint or method");
            connection->set_body(error.dump());
            connection->set_status(status_code::not_found);
        }
    } catch (const std::exception& error) {
        const auto payload = create_error_payload("", error_e::internal_error, error.what());
        connection->set_body(payload.dump());
        connection->set_status(status_code::internal_server_error);
    }
}

void web_server_impl::handle_api_v1_get_node(const server_t::connection_ptr& connection,
                                             std::string_view                encoded_id) const
{
    handle_keyed_json_get(connection, config_getters_.node, encoded_id, "Node config");
}

void web_server_impl::handle_api_v1_get_node_status(const server_t::connection_ptr& connection,
                                                    std::string_view                encoded_id) const
{
    handle_keyed_json_get(connection, config_getters_.node_status, encoded_id, "Node status");
}

void web_server_impl::handle_api_v1_get_config(const server_t::connection_ptr& connection) const
{
    using namespace websocketpp::http;

    if (!config_getters_.node_config) {
        const auto error = create_error_payload("", error_e::internal_error, "Config service not available");
        connection->set_body(error.dump());
        connection->set_status(status_code::service_unavailable);
        return;
    }

    try {
        const nlohmann::json config = config_getters_.node_config();
        connection->set_body(config.dump());
        connection->set_status(status_code::ok);
    } catch (const std::exception& error) {
        const auto payload = create_error_payload("", error_e::internal_error, error.what());
        connection->set_body(payload.dump());
        connection->set_status(status_code::internal_server_error);
    }
}

void web_server_impl::handle_api_v1_post_control(const server_t::connection_ptr& connection)
{
    using namespace websocketpp::http;

    const std::string& body = connection->get_request_body();
    if (body.empty()) {
        const auto error = create_error_payload("", error_e::malformed_payload, "Request body is required");
        connection->set_body(error.dump());
        connection->set_status(status_code::bad_request);
        return;
    }

    auto doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        const auto error = create_error_payload("", error_e::malformed_payload, "Invalid JSON in request body");
        connection->set_body(error.dump());
        connection->set_status(status_code::bad_request);
        return;
    }

    const auto action = get_action_from_payload(doc);
    if (!action.has_value()) {
        const auto error = create_error_payload("", error_e::malformed_payload, "Invalid action");
        connection->set_body(error.dump());
        connection->set_status(status_code::bad_request);
        return;
    }
    if (*action != action_e::command) {
        const auto error =
            create_error_payload("", error_e::malformed_payload, "Only command actions are supported via HTTP API");
        connection->set_body(error.dump());
        connection->set_status(status_code::bad_request);
        return;
    }

    if (!get_topic_from_payload(doc).has_value()) {
        const auto error = create_error_payload("", error_e::invalid_topic, "Invalid topic");
        connection->set_body(error.dump());
        connection->set_status(status_code::bad_request);
        return;
    }

    const auto error_code = handle_user_command(std::move(doc), -1);
    if (error_code != error_e::no_error) {
        const bool             invalid_topic = error_code == error_e::invalid_topic;
        const std::string_view message       = invalid_topic ? "Invalid topic" : "Topic service not available";
        connection->set_status(invalid_topic ? status_code::bad_request : status_code::service_unavailable);
        connection->set_body(create_error_payload("", error_code, message).dump());
        return;
    }

    connection->set_status(status_code::no_content);
}

} // namespace miximus::web_server::detail
