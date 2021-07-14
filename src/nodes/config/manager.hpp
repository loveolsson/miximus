#pragma once
#include "nodes/node_type.hpp"
#include "types/connection_set.hpp"
#include "types/error.hpp"
#include "types/node_map.hpp"

#include <nlohmann/json_fwd.hpp>

#include <mutex>
#include <vector>

namespace miximus::nodes {

class node_manager
{
  public:
    class adapter_i
    {
      public:
        adapter_i()          = default;
        virtual ~adapter_i() = default;

        virtual void
                     emit_add_node(node_type_e type, std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
        virtual void emit_remove_node(std::string_view id, int64_t client_id)                                = 0;
        virtual void emit_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
        virtual void emit_add_connection(const connection& con, int64_t client_id)                           = 0;
        virtual void emit_remove_connection(const connection& con, int64_t client_id)                        = 0;
    };

  private:
    typedef std::vector<std::unique_ptr<adapter_i>> adapter_list_t;

    std::mutex     nodes_mutex_;
    node_map_t     nodes_;
    con_set_t      connections_;
    adapter_list_t adapters_;

  public:
    node_manager()  = default;
    ~node_manager() = default;

    error_e handle_add_node(node_type_e type, const std::string& id, const nlohmann::json& options, int64_t client_id);
    error_e handle_remove_node(const std::string& id, int64_t client_id);
    error_e handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id);
    error_e handle_add_connection(connection con, int64_t client_id);
    error_e handle_remove_connection(const connection& con, int64_t client_id, bool do_lock = true);

    nlohmann::json get_config();
    void           set_config(const nlohmann::json&);

    node_map_t clone_nodes();
    void       add_adapter(std::unique_ptr<adapter_i>&& adapter);
    void       clear_adapters();
};

} // namespace miximus::nodes