#pragma once
#include "core/app_state.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <mutex>
#include <string_view>
#include <vector>

namespace miximus::core {

class node_manager_s
{
  public:
    class adapter_i
    {
      public:
        adapter_i()          = default;
        virtual ~adapter_i() = default;

        virtual void emit_add_node(nodes::node_i::type_e type,
                                   std::string_view      id,
                                   const nlohmann::json& options,
                                   int64_t               client_id)                                                        = 0;
        virtual void emit_remove_node(std::string_view id, int64_t client_id)                                = 0;
        virtual void emit_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
        virtual void emit_add_connection(const nodes::connection_s& con, int64_t client_id)                  = 0;
        virtual void emit_remove_connection(const nodes::connection_s& con, int64_t client_id)               = 0;
    };

  private:
    typedef std::vector<std::unique_ptr<adapter_i>> adapter_list_t;

    std::mutex        nodes_mutex_;
    nodes::node_map_t nodes_;
    nodes::node_map_t nodes_copy_;
    nodes::con_set_t  connections_;
    adapter_list_t    adapters_;

  public:
    node_manager_s()  = default;
    ~node_manager_s() = default;

    error_e handle_add_node(nodes::node_i::type_e type,
                            const std::string&    id,
                            const nlohmann::json& options,
                            int64_t               client_id);
    error_e handle_remove_node(const std::string& id, int64_t client_id);
    error_e handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id);
    error_e handle_add_connection(nodes::connection_s con, int64_t client_id);
    error_e handle_remove_connection(const nodes::connection_s& con, int64_t client_id, bool do_lock = true);

    nlohmann::json get_config();
    void           set_config(const nlohmann::json&);

    void add_adapter(std::unique_ptr<adapter_i>&& adapter);
    void clear_adapters();

    void tick_one_frame(app_state_s&);
};

} // namespace miximus::core