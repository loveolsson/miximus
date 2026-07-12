#pragma once
#include "core/node_manager.hpp"
#include "render/font/font_registry_fwd.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace miximus::core {

class websocket_config_s : public node_manager_s::adapter_i
{
    node_manager_s&          manager_;
    web_server::server_s&    server_;
    render::font_registry_s& font_registry_;

    void handle_add_node(const nlohmann::json& msg, int64_t client_id);
    void handle_remove_node(const nlohmann::json& msg, int64_t client_id);
    void handle_update_node(const nlohmann::json& msg, int64_t client_id);
    void handle_add_connection(const nlohmann::json& msg, int64_t client_id);
    void handle_remove_connection(const nlohmann::json& msg, int64_t client_id);
    void handle_config(const nlohmann::json& msg, int64_t client_id);
    void handle_node_status(const nlohmann::json& msg, int64_t client_id);
    void handle_font_registry(const nlohmann::json& msg, int64_t client_id);

    void
    emit_add_node(std::string_view type, std::string_view id, const nlohmann::json& options, int64_t client_id) final;
    void emit_remove_node(std::string_view id, int64_t client_id) final;
    void emit_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id) final;
    void emit_add_connection(const nodes::connection_s& con, int64_t client_id) final;
    void emit_remove_connection(const nodes::connection_s& con, int64_t client_id) final;
    void emit_node_status(std::string_view id, const nlohmann::json& status) final;

  public:
    websocket_config_s(node_manager_s& manager, web_server::server_s& server, render::font_registry_s& font_registry);
    ~websocket_config_s() = default;
};
} // namespace miximus::core
