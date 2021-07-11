#pragma once
#include "messages/types.hpp"
#include "nodes/config/adapter.hpp"
#include "nodes/config/config.hpp"
#include "nodes/connection.hpp"

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
    node_manager();
    ~node_manager();

    message::error_e
                     handle_add_node(node_type_e type, const std::string& id, const nlohmann::json& options, int64_t client_id);
    message::error_e handle_remove_node(const std::string& id, int64_t client_id);
    message::error_e handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id);
    message::error_e handle_add_connection(const connection& con, int64_t client_id);
    message::error_e handle_remove_connection(const connection& con, int64_t client_id);

    nlohmann::json get_config();

    node_cfg clone_node_config();
    void     add_adapter(std::unique_ptr<config_adapter>&& adapter);
};

} // namespace miximus::nodes