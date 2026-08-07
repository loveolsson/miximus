#include "core/adapters/adapter_websocket.hpp"

#include "core/configuration.hpp"
#include "logger/logger.hpp"
#include "render/font/font_registry.hpp"
#include "web_server/server.hpp"
#include "web_server/typed_server.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace miximus::core {
namespace {

std::optional<origin_info_s> make_origin_info(int64_t origin_id, const std::optional<std::string>& origin_token)
{
    return origin_info_s{.id = origin_id, .token = origin_token};
}

std::optional<int64_t> get_origin_id(const std::optional<origin_info_s>& origin)
{
    return origin ? std::optional{origin->id} : std::nullopt;
}

std::optional<std::string> get_origin_token(const std::optional<origin_info_s>& origin)
{
    return origin ? origin->token : std::nullopt;
}

class websocket_config_s final : public node_manager_s::adapter_i
{
    // These are required, non-owning dependencies whose lifetime is managed by app_state_s.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    node_manager_s&          manager_;
    configuration_s&         configuration_;
    web_server::server_s&    server_;
    render::font_registry_s& font_registry_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

    void handle_add_node(const web_message::add_node_request_s& message, int64_t origin_id);
    void handle_remove_node(const web_message::remove_node_request_s& message, int64_t origin_id);
    void handle_update_node(const web_message::update_node_request_s& message, int64_t origin_id);
    void handle_add_connection(const web_message::add_connection_request_s& message, int64_t origin_id);
    void handle_remove_connection(const web_message::remove_connection_request_s& message, int64_t origin_id);
    void handle_config(const web_message::config_request_s& message, int64_t origin_id);
    void handle_node_status(const web_message::node_status_request_s& message, int64_t origin_id);
    void handle_font_registry(const web_message::font_registry_request_s& message, int64_t origin_id);

    void emit_add_node(std::string_view                    type,
                       std::string_view                    id,
                       const nlohmann::json&               options,
                       const std::optional<origin_info_s>& origin) final;
    void emit_remove_node(std::string_view id, const std::optional<origin_info_s>& origin) final;
    void emit_update_node(std::string_view                    id,
                          const nlohmann::json&               options,
                          bool                                has_corrected_values,
                          const std::optional<origin_info_s>& origin) final;
    void emit_add_connection(const connection_s& con, const std::optional<origin_info_s>& origin) final;
    void emit_remove_connection(const connection_s& con, const std::optional<origin_info_s>& origin) final;
    void emit_node_status(std::string_view id, const nlohmann::json& status) final;

  public:
    websocket_config_s(node_manager_s&          manager,
                       configuration_s&         configuration,
                       web_server::server_s&    server,
                       render::font_registry_s& font_registry);
};

websocket_config_s::websocket_config_s(node_manager_s&          manager,
                                       configuration_s&         configuration,
                                       web_server::server_s&    server,
                                       render::font_registry_s& font_registry)
    : manager_(manager)
    , configuration_(configuration)
    , server_(server)
    , font_registry_(font_registry)
{
    server_.subscribe<web_message::add_node_request_s>(topic_e::add_node,
                                                       std::bind_front(&websocket_config_s::handle_add_node, this));
    server_.subscribe<web_message::remove_node_request_s>(
        topic_e::remove_node, std::bind_front(&websocket_config_s::handle_remove_node, this));
    server_.subscribe<web_message::add_connection_request_s>(
        topic_e::add_connection, std::bind_front(&websocket_config_s::handle_add_connection, this));
    server_.subscribe<web_message::remove_connection_request_s>(
        topic_e::remove_connection, std::bind_front(&websocket_config_s::handle_remove_connection, this));
    server_.subscribe<web_message::update_node_request_s>(
        topic_e::update_node, std::bind_front(&websocket_config_s::handle_update_node, this));
    server_.subscribe<web_message::font_registry_request_s>(
        topic_e::font_registry, std::bind_front(&websocket_config_s::handle_font_registry, this));
    server_.subscribe<web_message::config_request_s>(topic_e::config,
                                                     std::bind_front(&websocket_config_s::handle_config, this));
    server_.subscribe<web_message::node_status_request_s>(
        topic_e::node_status, std::bind_front(&websocket_config_s::handle_node_status, this));
}

void websocket_config_s::handle_font_registry(const web_message::font_registry_request_s& message, int64_t origin_id)
{
    const auto token = message.token.value_or("");

    try {
        switch (message.command) {
            case font_registry_command_e::refresh:
                font_registry_.refresh();
                break;
        }
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } catch (const std::exception& e) {
        getlog("http")->error("Failed to refresh font registry: {}", e.what());
        server_.send_message_sync(web_message::error_s{.token = token, .error = error_e::internal_error}, origin_id);
    }
}

void websocket_config_s::handle_add_node(const web_message::add_node_request_s& message, int64_t origin_id)
{
    const auto token  = message.token.value_or("");
    const auto origin = make_origin_info(origin_id, message.token);
    const auto result = manager_.handle_add_node(message.type, message.options, origin);
    if (result == error_e::no_error) {
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } else {
        server_.send_message_sync(web_message::error_s{.token = token, .error = result}, origin_id);
    }
}

void websocket_config_s::handle_remove_node(const web_message::remove_node_request_s& message, int64_t origin_id)
{
    const auto token  = message.token.value_or("");
    const auto origin = make_origin_info(origin_id, message.token);
    const auto result = manager_.handle_remove_node(message.id, origin);
    if (result == error_e::no_error) {
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } else {
        server_.send_message_sync(web_message::error_s{.token = token, .error = result}, origin_id);
    }
}

void websocket_config_s::handle_update_node(const web_message::update_node_request_s& message, int64_t origin_id)
{
    const auto token  = message.token.value_or("");
    const auto origin = make_origin_info(origin_id, message.token);
    const auto result = manager_.handle_update_node(message.id, message.options, origin);
    if (result.error == error_e::no_error) {
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } else {
        server_.send_message_sync(web_message::error_s{.token = token, .error = result.error}, origin_id);
    }
}

void websocket_config_s::handle_add_connection(const web_message::add_connection_request_s& message, int64_t origin_id)
{
    const auto token  = message.token.value_or("");
    const auto origin = make_origin_info(origin_id, message.token);
    const auto result = manager_.handle_add_connection(message.connection, origin);
    if (result == error_e::no_error) {
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } else {
        server_.send_message_sync(web_message::error_s{.token = token, .error = result}, origin_id);
    }
}

void websocket_config_s::handle_remove_connection(const web_message::remove_connection_request_s& message,
                                                  int64_t                                         origin_id)
{
    const auto token  = message.token.value_or("");
    const auto origin = make_origin_info(origin_id, message.token);
    const auto result = manager_.handle_remove_connection(message.connection, origin);
    if (result == error_e::no_error) {
        server_.send_message_sync(web_message::result_s{.token = token}, origin_id);
    } else {
        server_.send_message_sync(web_message::error_s{.token = token, .error = result}, origin_id);
    }
}

void websocket_config_s::handle_config(const web_message::config_request_s& message, int64_t origin_id)
{
    const auto token = message.token.value_or("");
    server_.send_message_sync(
        web_message::config_result_s{
            .token  = token,
            .config = configuration_.get_snapshot().get<web_message::config_s>(),
        },
        origin_id);
}

void websocket_config_s::handle_node_status(const web_message::node_status_request_s& message, int64_t origin_id)
{
    const auto token = message.token.value_or("");
    server_.send_message_sync(
        web_message::node_status_result_s{
            .token  = token,
            .id     = message.id,
            .status = manager_.get_node_status(message.id),
        },
        origin_id);
}

void websocket_config_s::emit_add_node(std::string_view                    type,
                                       std::string_view                    id,
                                       const nlohmann::json&               options,
                                       const std::optional<origin_info_s>& origin)
{
    server_.broadcast_message_sync(web_message::add_node_command_s{
        .origin_id    = get_origin_id(origin),
        .origin_token = get_origin_token(origin),
        .node         = {.type = std::string(type), .id = std::string(id), .options = options},
    });
}

void websocket_config_s::emit_remove_node(std::string_view id, const std::optional<origin_info_s>& origin)
{
    server_.broadcast_message_sync(web_message::remove_node_command_s{
        .origin_id    = get_origin_id(origin),
        .origin_token = get_origin_token(origin),
        .id           = std::string(id),
    });
}

void websocket_config_s::emit_update_node(std::string_view                    id,
                                          const nlohmann::json&               options,
                                          bool                                has_corrected_values,
                                          const std::optional<origin_info_s>& origin)
{
    server_.broadcast_message_sync(web_message::update_node_command_s{
        .origin_id            = get_origin_id(origin),
        .origin_token         = get_origin_token(origin),
        .id                   = std::string(id),
        .options              = options,
        .has_corrected_values = has_corrected_values,
    });
}

void websocket_config_s::emit_add_connection(const connection_s& con, const std::optional<origin_info_s>& origin)
{
    server_.broadcast_message_sync(web_message::add_connection_command_s{
        .origin_id    = get_origin_id(origin),
        .origin_token = get_origin_token(origin),
        .connection   = con,
    });
}

void websocket_config_s::emit_remove_connection(const connection_s& con, const std::optional<origin_info_s>& origin)
{
    server_.broadcast_message_sync(web_message::remove_connection_command_s{
        .origin_id    = get_origin_id(origin),
        .origin_token = get_origin_token(origin),
        .connection   = con,
    });
}

void websocket_config_s::emit_node_status(std::string_view id, const nlohmann::json& status)
{
    server_.broadcast_message_sync(web_message::node_status_command_s{
        .id     = std::string(id),
        .status = status,
    });
}

} // namespace

std::unique_ptr<node_manager_s::adapter_i> create_websocket_adapter(node_manager_s&          manager,
                                                                    configuration_s&         configuration,
                                                                    web_server::server_s&    server,
                                                                    render::font_registry_s& font_registry)
{
    return std::make_unique<websocket_config_s>(manager, configuration, server, font_registry);
}

} // namespace miximus::core
