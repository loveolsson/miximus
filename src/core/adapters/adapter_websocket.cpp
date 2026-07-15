#include "core/adapters/adapter_websocket.hpp"

#include "core/configuration.hpp"
#include "logger/logger.hpp"
#include "render/font/font_registry.hpp"
#include "web_server/payload_create.hpp"
#include "web_server/payload_parse.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <functional>
#include <string_view>

namespace {
using nlohmann::json;
using namespace miximus;

json create_add_node_command_payload(std::string_view type, std::string_view id, const json& options, int64_t origin_id)
{
    return {
        {"action",    enum_to_string(action_e::command)},
        {"topic",     enum_to_string(topic_e::add_node)},
        {"origin_id", origin_id                        },
        {"node",
         {
             {"type", type},
             {"id", id},
             {"options", options},
         }                                             },
    };
}

json create_remove_node_command_payload(std::string_view id, int64_t origin_id)
{
    return {
        {"action",    enum_to_string(action_e::command)   },
        {"topic",     enum_to_string(topic_e::remove_node)},
        {"origin_id", origin_id                           },
        {"id",        id                                  },
    };
}

json create_update_node_command_payload(std::string_view id,
                                        const json&      options,
                                        bool             has_corrected_values,
                                        int64_t          origin_id)
{
    return {
        {"action",               enum_to_string(action_e::command)   },
        {"topic",                enum_to_string(topic_e::update_node)},
        {"origin_id",            origin_id                           },
        {"id",                   id                                  },
        {"options",              options                             },
        {"has_corrected_values", has_corrected_values                },
    };
}

json create_add_connection_command_payload(const miximus::nodes::connection_s& connection, int64_t origin_id)
{
    return {
        {"action",     enum_to_string(action_e::command)      },
        {"topic",      enum_to_string(topic_e::add_connection)},
        {"origin_id",  origin_id                              },
        {"connection", connection                             },
    };
}

json create_remove_connection_command_payload(const miximus::nodes::connection_s& connection, int64_t origin_id)
{
    return {
        {"action",     enum_to_string(action_e::command)         },
        {"topic",      enum_to_string(topic_e::remove_connection)},
        {"origin_id",  origin_id                                 },
        {"connection", connection                                },
    };
}

json create_node_status_command_payload(std::string_view id, const json& status)
{
    return {
        {"action", enum_to_string(action_e::command)   },
        {"topic",  enum_to_string(topic_e::node_status)},
        {"id",     id                                  },
        {"status", status                              },
    };
}
} // namespace

namespace miximus::core {

using nlohmann::json;
using namespace web_server;

websocket_config_s::websocket_config_s(node_manager_s&          manager,
                                       configuration_s&         configuration,
                                       web_server::server_s&    server,
                                       render::font_registry_s& font_registry)
    : manager_(manager)
    , configuration_(configuration)
    , server_(server)
    , font_registry_(font_registry)
{
    server_.subscribe(topic_e::add_node, std::bind_front(&websocket_config_s::handle_add_node, this));
    server_.subscribe(topic_e::remove_node, std::bind_front(&websocket_config_s::handle_remove_node, this));
    server_.subscribe(topic_e::add_connection, std::bind_front(&websocket_config_s::handle_add_connection, this));
    server_.subscribe(topic_e::remove_connection, std::bind_front(&websocket_config_s::handle_remove_connection, this));
    server_.subscribe(topic_e::update_node, std::bind_front(&websocket_config_s::handle_update_node, this));
    server_.subscribe(topic_e::font_registry, std::bind_front(&websocket_config_s::handle_font_registry, this));
    server_.subscribe(topic_e::config, std::bind_front(&websocket_config_s::handle_config, this));
    server_.subscribe(topic_e::node_status, std::bind_front(&websocket_config_s::handle_node_status, this));
}

void websocket_config_s::handle_font_registry(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        const auto command = msg.at("command").get<std::string_view>();
        if (command != "refresh") {
            server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
            return;
        }

        font_registry_.refresh();
        server_.send_message_sync(create_result_payload(token), client_id);
    } catch (const json::exception& e) {
        getlog("http")->warn("Received malformed font registry command: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    } catch (const std::exception& e) {
        getlog("http")->error("Failed to refresh font registry: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::internal_error), client_id);
    }
}

void websocket_config_s::handle_add_node(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& node_obj = msg.at("node");

        auto  type    = node_obj.at("type").get<std::string_view>();
        auto  id      = node_obj.at("id").get<std::string_view>();
        auto& options = node_obj.at("options");

        auto res = manager_.handle_add_node(type, id, options, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_payload(token, res), client_id);
        }
    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for create node: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_remove_node(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto id = msg.at("id").get<std::string_view>();

        auto res = manager_.handle_remove_node(id, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for remove node: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_update_node(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto  id      = msg.at("id").get<std::string_view>();
        auto& options = msg.at("options");

        auto result = manager_.handle_update_node(id, options, client_id);

        if (result.error == error_e::no_error) {
            server_.send_message_sync(create_result_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_payload(token, result.error), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for update node: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_add_connection(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& con_obj = msg.at("connection");
        auto  con     = con_obj.get<nodes::connection_s>();

        auto res = manager_.handle_add_connection(con, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for add connection: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_remove_connection(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        auto& con_obj = msg.at("connection");
        auto  con     = con_obj.get<nodes::connection_s>();

        auto res = manager_.handle_remove_connection(con, client_id);

        if (res == error_e::no_error) {
            server_.send_message_sync(create_result_payload(token), client_id);
        } else {
            server_.send_message_sync(create_error_payload(token, res), client_id);
        }

    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for remove connection: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::handle_config(const json& msg, int64_t client_id)
{
    const auto token = get_token_from_payload(msg);
    server_.send_message_sync(create_config_result_payload(token, configuration_.get_snapshot()), client_id);
}

void websocket_config_s::handle_node_status(const json& msg, int64_t client_id)
{
    auto token = get_token_from_payload(msg);

    try {
        const auto id = msg.at("id").get<std::string_view>();
        server_.send_message_sync(create_node_status_result_payload(token, id, manager_.get_node_status(id)),
                                  client_id);
    } catch (json::exception& e) {
        getlog("http")->warn("Received malformed payload for node_status: {}", e.what());
        server_.send_message_sync(create_error_payload(token, error_e::malformed_payload), client_id);
    }
}

void websocket_config_s::emit_add_node(std::string_view type,
                                       std::string_view id,
                                       const json&      options,
                                       int64_t          client_id)
{
    server_.broadcast_message_sync(create_add_node_command_payload(type, id, options, client_id));
}

void websocket_config_s::emit_remove_node(std::string_view id, int64_t client_id)
{
    server_.broadcast_message_sync(create_remove_node_command_payload(id, client_id));
}

void websocket_config_s::emit_update_node(std::string_view id,
                                          const json&      options,
                                          bool             has_corrected_values,
                                          int64_t          client_id)
{
    server_.broadcast_message_sync(create_update_node_command_payload(id, options, has_corrected_values, client_id));
}

void websocket_config_s::emit_add_connection(const nodes::connection_s& con, int64_t client_id)
{
    server_.broadcast_message_sync(create_add_connection_command_payload(con, client_id));
}

void websocket_config_s::emit_remove_connection(const nodes::connection_s& con, int64_t client_id)
{
    server_.broadcast_message_sync(create_remove_connection_command_payload(con, client_id));
}

void websocket_config_s::emit_node_status(std::string_view id, const nlohmann::json& status)
{
    server_.broadcast_message_sync(create_node_status_command_payload(id, status));
}

} // namespace miximus::core
