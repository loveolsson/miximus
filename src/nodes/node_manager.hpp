#pragma once
#include "messages/types.hpp"
#include "nodes/node_config.hpp"
#include "web_server/web_server.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace miximus {

class node_manager
{
    typedef nlohmann::json json;

    web_server::web_server* server_;

    std::shared_mutex config_mutex_;

    std::shared_mutex nodes_mutex_;
    nodes::node_cfg_t config_;

    void handle_add_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_remove_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void hande_update_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_add_connection(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_remove_connection(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_config(json&& msg, int64_t client_id, web_server::response_fn_t cb);

    std::shared_ptr<nodes::node> create_node(const std::string& type, const std::string& id, message::error_t& error);

    json get_config();

  public:
    node_manager();
    ~node_manager();

    void              make_server_subscriptions(web_server::web_server& server);
    nodes::node_cfg_t clone_node_config();
};

} // namespace miximus