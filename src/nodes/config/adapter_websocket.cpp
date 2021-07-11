#include "nodes/config/adapter_websocket.hpp"
#include "logger/logger.hpp"
#include "messages/payload.hpp"
#include "messages/templates.hpp"
#include "nodes/config/manager.hpp"
#include "utils/bind.hpp"

#include <cassert>
#include <string>

namespace miximus::nodes {
using namespace message;
using namespace nlohmann;

websocket_config::websocket_config(web_server::server& server)
    : server_(server)
{
    server_.subscribe(topic_e::add_node, utils::bind(&websocket_config::handle_add_node, this));
    server_.subscribe(topic_e::remove_node, utils::bind(&websocket_config::handle_remove_node, this));
    server_.subscribe(topic_e::add_connection, utils::bind(&websocket_config::handle_add_connection, this));
    server_.subscribe(topic_e::remove_connection, utils::bind(&websocket_config::handle_remove_connection, this));
    server_.subscribe(topic_e::update_node, utils::bind(&websocket_config::handle_update_node, this));
    server_.subscribe(topic_e::config, utils::bind(&websocket_config::handle_config, this));
}

websocket_config::~websocket_config()
{
    server_.subscribe(topic_e::add_node, nullptr);
    server_.subscribe(topic_e::remove_node, nullptr);
    server_.subscribe(topic_e::add_connection, nullptr);
    server_.subscribe(topic_e::remove_connection, nullptr);
    server_.subscribe(topic_e::update_node, nullptr);
    server_.subscribe(topic_e::config, nullptr);
}

void websocket_config::handle_add_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& node_obj = msg["node"];

        std::string type_str = node_obj["type"];
        auto        type     = type_from_string(type_str);
        std::string id       = node_obj["id"];
        auto&       options  = node_obj["options"];

        auto res = manager_->handle_add_node(type, id, options, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }
    } catch (json::exception& e) {
        spdlog::get("http")->warn("Received malformed payload for create node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config::handle_remove_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        std::string id = msg["id"];

        auto res = manager_->handle_remove_node(id, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        spdlog::get("http")->warn("Received malformed payload for remove node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config::handle_update_node(json&& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        std::string id      = msg["id"];
        auto&       options = msg["options"];

        spdlog::get("http")->info(R"(Updating node with id "{}")", id);

        auto res = manager_->handle_update_node(id, options, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_base_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_base_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        spdlog::get("http")->warn("Received malformed payload for update node: {}", e.what());
        server_.send_message_sync(create_error_base_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config::handle_add_connection(json&& msg, int64_t client_id) {}

void websocket_config::handle_remove_connection(json&& msg, int64_t client_id) {}

void websocket_config::handle_config(json&& msg, int64_t client_id)
{
    auto token    = get_token_from_payload(msg);
    auto response = create_result_base_payload(token);

    response["config"] = manager_->get_config();

    server_.send_message_sync(response, client_id);
}

void websocket_config::emit_add_node(node_type_e type, std::string_view id, const json& options, int64_t client_id)
{
    auto payload         = create_command_base_payload(topic_e::add_node);
    payload["origin_id"] = client_id;

    payload["node"] = {
        {"type", type_to_string(type)},
        {"id", id},
        {"options", options},
    };

    server_.broadcast_message_sync(payload);
}

void websocket_config::emit_remove_node(std::string_view id, int64_t client_id)
{
    auto payload         = create_command_base_payload(topic_e::remove_node);
    payload["origin_id"] = client_id;
    payload["id"]        = id;

    server_.broadcast_message_sync(payload);
}

void websocket_config::emit_update_node(std::string_view id, const json& options, int64_t client_id)
{
    auto payload         = create_command_base_payload(topic_e::update_node);
    payload["origin_id"] = client_id;
    payload["id"]        = id;
    payload["options"]   = options;

    server_.broadcast_message_sync(payload);
}

void websocket_config::emit_add_connection(const connection& con, int64_t client_id) {}

void websocket_config::emit_remove_connection(const connection& con, int64_t client_id) {}

} // namespace miximus::nodes