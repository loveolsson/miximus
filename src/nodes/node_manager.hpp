#pragma once
#include "messages/types.hpp"
#include "nodes/node_config.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json_fwd.hpp>

#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace miximus {

class node_manager
{
    typedef nlohmann::json json;

    web_server::server* server_;

    std::shared_mutex config_mutex_;

    std::shared_mutex nodes_mutex_;
    nodes::node_cfg   config_;

    void handle_add_node(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);
    void handle_remove_node(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);
    void hande_update_node(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);
    void handle_add_connection(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);
    void handle_remove_connection(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);
    void handle_config(json&& msg, int64_t client_id, const web_server::response_fn_t& cb);

    static std::shared_ptr<nodes::node> create_node(const std::string& type, message::error_e& error);

    json get_config();

  public:
    node_manager();
    ~node_manager() = default;

    void            make_server_subscriptions(web_server::server& server);
    nodes::node_cfg clone_node_config();
};

} // namespace miximus