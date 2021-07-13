#pragma once
#include "nodes/config/adapter.hpp"
#include "nodes/config/config.hpp"
#include "types/connection.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace miximus::nodes {

class node_manager
{
    std::mutex                                   nodes_mutex_;
    node_cfg                                     config_;
    std::vector<std::unique_ptr<config_adapter>> adapters_;

  public:
    node_manager() = default;
    ~node_manager();

    error_e handle_add_node(node_type_e type, const std::string& id, const nlohmann::json& options, int64_t client_id);
    error_e handle_remove_node(const std::string& id, int64_t client_id);
    error_e handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id);
    error_e handle_add_connection(connection con, int64_t client_id);
    error_e handle_remove_connection(const connection& con, int64_t client_id, bool do_lock = true);

    nlohmann::json get_config();

    node_cfg clone_node_config();
    void     add_adapter(std::unique_ptr<config_adapter>&& adapter);
    void     clear_adapters();
};

} // namespace miximus::nodes