#pragma once
#include "nodes/node_type.hpp"
#include "types/connection.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace miximus::nodes {

class node_manager;

class config_adapter
{
  protected:
    node_manager* manager_{};

  public:
    config_adapter()          = default;
    virtual ~config_adapter() = default;

    virtual void
                 emit_add_node(node_type_e type, std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
    virtual void emit_remove_node(std::string_view id, int64_t client_id)                                = 0;
    virtual void emit_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
    virtual void emit_add_connection(const connection& con, int64_t client_id)                           = 0;
    virtual void emit_remove_connection(const connection& con, int64_t client_id)                        = 0;

    void set_manager(node_manager* manager) { manager_ = manager; }
};
} // namespace miximus::nodes