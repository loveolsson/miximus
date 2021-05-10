#pragma once
#include "messages/types.hpp"
#include "nodes/node.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace miximus {
namespace web_server {
class web_server;
}

class node_manager
{
    typedef nlohmann::json json;

    web_server::web_server* server_;

    std::shared_mutex           config_mutex_;
    std::map<std::string, json> node_config_;
    std::map<std::string, json> con_config_;

    std::shared_mutex                                      nodes_mutex_;
    std::unordered_map<std::string, std::shared_ptr<node>> nodes_;

    void handle_add_node(message::action_t action, json&& msg, int64_t client_id);
    void handle_remove_node(message::action_t action, json&& msg, int64_t client_id);
    void hande_update_node(message::action_t action, json&& msg, int64_t client_id);
    void handle_add_connection(message::action_t action, json&& msg, int64_t client_id);
    void handle_remove_connection(message::action_t action, json&& msg, int64_t client_id);
    void handle_config(message::action_t action, json&& msg, int64_t client_id);

    void respond_success(int64_t client_id, std::string_view token);
    void respond_error(int64_t client_id, std::string_view token, message::error_t error);

    std::shared_ptr<node> create_node(const std::string& type, const std::string& id);

    json get_config();

  public:
    node_manager();
    ~node_manager();

    void make_server_subscriptions(web_server::web_server& server);

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