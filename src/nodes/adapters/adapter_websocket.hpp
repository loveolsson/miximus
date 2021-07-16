#pragma once
#include "nodes/manager.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace miximus::nodes {

class websocket_config_s : public node_manager_s::adapter_i
{
    node_manager_s&       manager_;
    web_server::server_s& server_;

    void handle_add_node(nlohmann::json&& msg, int64_t client_id);
    void handle_remove_node(nlohmann::json&& msg, int64_t client_id);
    void handle_update_node(nlohmann::json&& msg, int64_t client_id);
    void handle_add_connection(nlohmann::json&& msg, int64_t client_id);
    void handle_remove_connection(nlohmann::json&& msg, int64_t client_id);
    void handle_config(nlohmann::json&& msg, int64_t client_id);

  public:
    websocket_config_s(node_manager_s& manager, web_server::server_s& server);
    ~websocket_config_s();

    void emit_add_node(node_type_e type, std::string_view id, const nlohmann::json& options, int64_t client_id) final;
    void emit_remove_node(std::string_view id, int64_t client_id) final;
    void emit_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id) final;
    void emit_add_connection(const connection_s& con, int64_t client_id) final;
    void emit_remove_connection(const connection_s& con, int64_t client_id) final;
};
} // namespace miximus::nodes