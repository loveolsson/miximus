#include "core/adapters/adapter_websocket.hpp"
#include "logger/logger.hpp"
#include "utils/bind.hpp"
#include "web_server/payload.hpp"
#include "web_server/templates.hpp"

namespace miximus::core {

using nlohmann::json;
using namespace web_server;

websocket_config_s::websocket_config_s(node_manager_s& manager, web_server::server_s& server)
    : manager_(manager)
    , server_(server)
{
    server_.subscribe(topic_e::add_node, utils::bind(&websocket_config_s::handle_add_node, this));
    server_.subscribe(topic_e::remove_node, utils::bind(&websocket_config_s::handle_remove_node, this));
    server_.subscribe(topic_e::add_connection, utils::bind(&websocket_config_s::handle_add_connection, this));
    server_.subscribe(topic_e::remove_connection, utils::bind(&websocket_config_s::handle_remove_connection, this));
    server_.subscribe(topic_e::update_node, utils::bind(&websocket_config_s::handle_update_node, this));
    server_.subscribe(topic_e::config, utils::bind(&websocket_config_s::handle_config, this));
}

void websocket_config_s::handle_add_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& node_obj = msg.at("node");

        auto  type    = node_obj.at("type").get<std::string_view>();
        auto  id      = node_obj.at("id").get<std::string_view>();
        auto& options = node_obj.at("options");

        auto res = manager_.handle_add_node(type, id, options, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }
    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for create node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_remove_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto id = msg.at("id").get<std::string_view>();

        auto res = manager_.handle_remove_node(id, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for remove node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_update_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto  id      = msg.at("id").get<std::string_view>();
        auto& options = msg.at("options");

        auto res = manager_.handle_update_node(id, options, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for update node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_add_connection(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& con_obj = msg.at("connection");
        auto  con     = con_obj.get<nodes::connection_s>();

        auto res = manager_.handle_add_connection(con, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for add connection: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_remove_connection(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& con_obj = msg.at("connection");
        auto  con     = con_obj.get<nodes::connection_s>();

        auto res = manager_.handle_remove_connection(con, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for remove connection: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_config(json&& msg, int64_t client_id)
{
    auto token    = get_token_from_payload(msg);
    auto response = create_result_base_payload(token);

    response["config"] = manager_.get_config();

    server_.send_message_sync(response, client_id);
}

void websocket_config_s::emit_add_node(std::string_view type,
                                       std::string_view id,
                                       const json&      options,
                                       int64_t          client_id)
{
    auto payload         = create_command_base_payload(topic_e::add_node);
    payload["origin_id"] = client_id;

    payload["node"] = {
        {"type", type},
        {"id", id},
        {"options", options},
    };

    server_.broadcast_message_sync(payload);
}

void websocket_config_s::emit_remove_node(std::string_view id, int64_t client_id)
{
    auto payload         = create_command_base_payload(topic_e::remove_node);
    payload["origin_id"] = client_id;
    payload["id"]        = id;

    server_.broadcast_message_sync(payload);
}

void websocket_config_s::emit_update_node(std::string_view id, const json& options, int64_t client_id)
{
    auto payload         = create_command_base_payload(topic_e::update_node);
    payload["origin_id"] = client_id;
    payload["id"]        = id;
    payload["options"]   = options;

    server_.broadcast_message_sync(payload);
}

void websocket_config_s::emit_add_connection(const nodes::connection_s& con, int64_t client_id)
{
    auto payload          = create_command_base_payload(topic_e::add_connection);
    payload["origin_id"]  = client_id;
    payload["connection"] = con;

    server_.broadcast_message_sync(payload);
}

void websocket_config_s::emit_remove_connection(const nodes::connection_s& con, int64_t client_id)
{
    auto payload          = create_command_base_payload(topic_e::remove_connection);
    payload["origin_id"]  = client_id;
    payload["connection"] = con;

    server_.broadcast_message_sync(payload);
}

} // namespace miximus::core