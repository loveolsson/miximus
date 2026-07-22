#pragma once
#include "core/app_state_fwd.hpp"
#include "core/application_settings.hpp"
#include "core/configuration_fwd.hpp"
#include "core/frame_scheduler_fwd.hpp"
#include "core/node_status_registry_fwd.hpp"
#include "nodes/node_fwd.hpp"
#include "nodes/node_map.hpp"
#include "nodes/option_result.hpp"
#include "nodes/register_all.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace miximus::core {

class node_manager_s
{
  public:
    class adapter_i
    {
        virtual void
        emit_add_node(std::string_view type, std::string_view id, const nlohmann::json& options, int64_t client_id) = 0;
        virtual void emit_remove_node(std::string_view id, int64_t client_id)                                       = 0;
        virtual void emit_update_node(std::string_view      id,
                                      const nlohmann::json& options,
                                      bool                  has_corrected_values,
                                      int64_t               client_id)                                                            = 0;
        virtual void emit_add_connection(const nodes::connection_s& con, int64_t client_id)                         = 0;
        virtual void emit_remove_connection(const nodes::connection_s& con, int64_t client_id)                      = 0;
        virtual void emit_node_status(std::string_view id, const nlohmann::json& status)                            = 0;

      public:
        adapter_i()          = default;
        virtual ~adapter_i() = default;

        friend class node_manager_s;
    };

  private:
    friend class configuration_s;

    using adapter_list_t = std::vector<std::unique_ptr<adapter_i>>;

    std::mutex                      nodes_mutex_;
    nodes::node_map_t               nodes_;
    nodes::node_map_t               nodes_copy_;
    std::unordered_set<std::string> dirty_nodes_;
    std::unordered_set<std::string> removed_nodes_;
    nodes::con_set_t                connections_;
    nodes::node_definition_map_t    node_definitions_;
    application_settings_s          application_settings_;
    adapter_list_t                  adapters_;
    node_status_registry_s*         status_registry_{nullptr};

    error_e remove_connection_locked(const nodes::connection_s& con, int64_t client_id);

  public:
    node_manager_s();
    ~node_manager_s() = default;

    error_e
    handle_add_node(std::string_view type, std::string_view id, const nlohmann::json& options, int64_t client_id);
    error_e handle_remove_node(std::string_view id, int64_t client_id);
    nodes::set_options_result_s
            handle_update_node(std::string_view id, const nlohmann::json& options, int64_t client_id);
    error_e handle_add_connection(nodes::connection_s con, int64_t client_id);
    error_e handle_remove_connection(const nodes::connection_s& con, int64_t client_id);

    nlohmann::json get_node_status(std::string_view id) const;
    nlohmann::json get_application_settings();

    void add_adapter(std::unique_ptr<adapter_i>&& adapter);
    void clear_adapters();

    void tick_one_frame(app_state_s*, frame_scheduler_s&);
    void clear_nodes(app_state_s*);

    std::pair<std::shared_ptr<nodes::node_i>, error_e> create_node(std::string_view type);
};

} // namespace miximus::core
