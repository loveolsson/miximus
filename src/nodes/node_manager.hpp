#pragma once
#include "messages/types.hpp"
#include "nodes/node.hpp"
#include "web_server/web_server.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace miximus {

class node_manager
{
    typedef nlohmann::json                                         json;
    typedef std::unordered_map<std::string, std::shared_ptr<node>> node_map_t;

    web_server::web_server* server_;

    std::shared_mutex           config_mutex_;
    std::map<std::string, json> node_config_;
    std::map<std::string, json> con_config_;

    std::shared_mutex nodes_mutex_;
    node_map_t        nodes_;

    void handle_add_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_remove_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void hande_update_node(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_add_connection(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_remove_connection(json&& msg, int64_t client_id, web_server::response_fn_t cb);
    void handle_config(json&& msg, int64_t client_id, web_server::response_fn_t cb);

    std::shared_ptr<node> create_node(const std::string& type, const std::string& id, message::error_t& error);

    json get_config();

  public:
    node_manager();
    ~node_manager();

    void       make_server_subscriptions(web_server::web_server& server);
    node_map_t clone_node_map();

    std::shared_ptr<node> find_node(const std::string& id);
};

inline std::shared_ptr<node> node_manager::find_node(const std::string& id)
{
    std::shared_lock lock(nodes_mutex_);

    auto it = nodes_.find(id);
    if (it != nodes_.end()) {
        return it->second;
    }

    return nullptr;
}

} // namespace miximus