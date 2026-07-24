#include "web_server/detail/path.hpp"
#include "web_server/detail/server_impl.hpp"
#include "web_server/payload_create.hpp"
#include "web_server/payload_parse.hpp"

#include <boost/url/segments_view.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view HTTP_GET     = "GET";
constexpr std::string_view HTTP_HEAD    = "HEAD";
constexpr std::string_view HTTP_POST    = "POST";
constexpr std::string_view HTTP_OPTIONS = "OPTIONS";

template <typename Connection>
void handle_keyed_json_get(const Connection&                               connection,
                           const miximus::web_server::keyed_json_getter_t& getter,
                           std::string_view                                id,
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

template <typename Connection>
bool prepare_api_route(const Connection& connection, std::string_view method, std::string_view route_method)
{
    using namespace websocketpp::http;

    const bool             is_get          = route_method == HTTP_GET;
    const bool             method_allowed  = method == route_method || (is_get && method == HTTP_HEAD);
    const std::string_view allowed_methods = is_get ? "GET, HEAD, OPTIONS" : "POST, OPTIONS";

    connection->replace_header("Allow", std::string(allowed_methods));
    connection->replace_header("Access-Control-Allow-Methods", std::string(allowed_methods));

    if (method == HTTP_OPTIONS) {
        connection->remove_header("Content-Type");
        connection->set_status(status_code::no_content);
        return false;
    }
    if (!method_allowed) {
        const auto error =
            miximus::web_server::create_error_payload("", miximus::error_e::internal_error, "Method not allowed");
        connection->set_body(error.dump());
        connection->set_status(status_code::method_not_allowed);
        return false;
    }
    return true;
}

} // namespace

namespace miximus::web_server::detail {

void web_server_impl::handle_api_request(const server_t::connection_ptr& connection,
                                         const std::string&              method,
                                         boost::urls::segments_view      api_path)
{
    using namespace websocketpp::http;

    connection->replace_header("Access-Control-Allow-Origin", "*");
    connection->replace_header("Access-Control-Allow-Headers", "Content-Type");
    connection->replace_header("Cache-Control", "no-store");
    connection->replace_header("Content-Type", "application/json");

    if (!path_starts_with(api_path, {"v1"})) {
        const auto error = create_error_payload("", error_e::not_found, "Invalid API endpoint");
        connection->set_body(error.dump());
        connection->set_status(status_code::not_found);
        return;
    }
    api_path = consume_segments(api_path, 1);

    try {
        if (path_matches(api_path, {"config"})) {
            if (prepare_api_route(connection, method, HTTP_GET)) {
                handle_api_v1_get_config(connection);
            }
            return;
        }

        if (path_matches(api_path, {"status"})) {
            if (prepare_api_route(connection, method, HTTP_GET)) {
                handle_api_v1_get_status(connection);
            }
            return;
        }

        if (path_matches(api_path, {"control"})) {
            if (prepare_api_route(connection, method, HTTP_POST)) {
                handle_api_v1_post_control(connection);
            }
            return;
        }

        if (path_starts_with(api_path, {"nodes"})) {
            handle_api_node_request(connection, method, consume_segments(api_path, 1));
            return;
        }

        const auto error = create_error_payload("", error_e::not_found, "Invalid API endpoint");
        connection->set_body(error.dump());
        connection->set_status(status_code::not_found);
    } catch (const std::exception& error) {
        const auto payload = create_error_payload("", error_e::internal_error, error.what());
        connection->set_body(payload.dump());
        connection->set_status(status_code::internal_server_error);
    }
}

void web_server_impl::handle_api_node_request(const server_t::connection_ptr& connection,
                                              const std::string&              method,
                                              boost::urls::segments_view      node_path)
{
    using namespace websocketpp::http;

    if (!node_path.empty()) {
        const auto node_id   = node_path.front();
        const auto remaining = consume_segments(node_path, 1);

        if (!node_id.empty() && remaining.empty()) {
            if (prepare_api_route(connection, method, HTTP_GET)) {
                handle_api_v1_get_node(connection, node_id);
            }
            return;
        }

        if (!node_id.empty() && path_matches(remaining, {"status"})) {
            if (prepare_api_route(connection, method, HTTP_GET)) {
                handle_api_v1_get_node_status(connection, node_id);
            }
            return;
        }
    }

    const auto error = create_error_payload("", error_e::not_found, "Invalid API endpoint");
    connection->set_body(error.dump());
    connection->set_status(status_code::not_found);
}

void web_server_impl::handle_api_v1_get_node(const server_t::connection_ptr& connection, std::string_view id) const
{
    handle_keyed_json_get(connection, config_getters_.node, id, "Node config");
}

void web_server_impl::handle_api_v1_get_node_status(const server_t::connection_ptr& connection,
                                                    std::string_view                id) const
{
    handle_keyed_json_get(connection, config_getters_.node_status, id, "Node status");
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

void web_server_impl::handle_api_v1_get_status(const server_t::connection_ptr& connection) const
{
    using namespace websocketpp::http;

    if (!config_getters_.node_statuses) {
        const auto error = create_error_payload("", error_e::internal_error, "Node status service not available");
        connection->set_body(error.dump());
        connection->set_status(status_code::service_unavailable);
        return;
    }

    try {
        connection->set_body(config_getters_.node_statuses().dump());
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

    connection->remove_header("Content-Type");
    connection->set_status(status_code::no_content);
}

} // namespace miximus::web_server::detail
