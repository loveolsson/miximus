#pragma once
#include "core/app_state_fwd.hpp"
#include "core/configuration_fwd.hpp"
#include "core/frame_scheduler_fwd.hpp"
#include "core/node_actions.hpp"
#include "core/node_status_handle.hpp"
#include "core/node_status_registry_fwd.hpp"
#include "core/origin_info.hpp"
#include "nodes/node_fwd.hpp"
#include "nodes/node_map.hpp"
#include "nodes/option_result.hpp"
#include "nodes/register_all.hpp"
#include "types/error.hpp"
#include "utils/observed_value.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace miximus::core {

class node_manager_s
{
    uint64_t gpu_recording_drops_{};

  public:
    class adapter_i
    {
        virtual void emit_add_node(std::string_view                    type,
                                   std::string_view                    id,
                                   const nlohmann::json&               options,
                                   const std::optional<origin_info_s>& origin)                                   = 0;
        virtual void emit_remove_node(std::string_view id, const std::optional<origin_info_s>& origin)           = 0;
        virtual void emit_update_node(std::string_view                    id,
                                      const nlohmann::json&               options,
                                      bool                                has_corrected_values,
                                      const std::optional<origin_info_s>& origin)                                = 0;
        virtual void emit_add_connection(const connection_s& con, const std::optional<origin_info_s>& origin)    = 0;
        virtual void emit_remove_connection(const connection_s& con, const std::optional<origin_info_s>& origin) = 0;

      public:
        adapter_i()          = default;
        virtual ~adapter_i() = default;

        friend class node_manager_s;
    };

  private:
    friend class configuration_s;
    friend struct node_manager_test_access_s;

    using adapter_list_t = std::vector<std::unique_ptr<adapter_i>>;

    // Pending records and settings share nodes_mutex_ and one frame cutoff.
    utils::unordered_string_map_t<node_actions_s::pending_s> pending_nodes_;
    bool                                                     actions_closed_{};
    size_t                                                   next_action_sequence_{};
    std::mutex                                               nodes_mutex_;
    nodes::node_map_t                                        nodes_;
    nodes::node_map_t                                        nodes_copy_;
    std::unordered_set<std::string>                          removed_nodes_;
    nodes::con_set_t                                         connections_;
    nodes::node_definition_map_t                             node_definitions_;
    adapter_list_t                                           adapters_;
    std::function<void()>                                    configuration_changed_;
    void                                                     checkpoint_configuration_locked();
    node_status_registry_s*                                  status_registry_{nullptr};
    node_status_handle_s                                     settings_status_handle_;
    utils::observed_value_s<uint64_t>                        reported_status_epoch_;

    node_actions_s::batch_t take_frame_updates();
    void                    close_actions();

    error_e handle_add_node_locked(std::string_view                    type,
                                   std::string_view                    id,
                                   const nlohmann::json&               options,
                                   const std::optional<origin_info_s>& origin);
    error_e remove_connection_locked(const connection_s& con, const std::optional<origin_info_s>& origin);

  public:
    explicit node_manager_s(node_status_registry_s* status_registry = nullptr);
    const node_status_handle_s& settings_status_handle() const { return settings_status_handle_; }
    ~node_manager_s();

    error_e handle_add_node(std::string_view                    type,
                            std::string_view                    id,
                            const nlohmann::json&               options,
                            const std::optional<origin_info_s>& origin = std::nullopt);
    error_e
    handle_add_node(std::string_view type, const nlohmann::json& options, const std::optional<origin_info_s>& origin);
    error_e handle_remove_node(std::string_view id, const std::optional<origin_info_s>& origin = std::nullopt);
    nodes::set_options_result_s handle_update_node(std::string_view                    id,
                                                   const nlohmann::json&               options,
                                                   const std::optional<origin_info_s>& origin = std::nullopt);
    error_e handle_add_connection(connection_s con, const std::optional<origin_info_s>& origin = std::nullopt);
    error_e handle_remove_connection(const connection_s&                 con,
                                     const std::optional<origin_info_s>& origin = std::nullopt);

    struct option_update_s
    {
        std::string    id;
        nlohmann::json options;
        // Supply for delayed native work that must not target an ID replacement.
        std::optional<node_handle_s> target{};
    };
    struct action_request_s
    {
        std::string                  id;
        std::string                  name;
        nlohmann::json               payload;
        node_actions_s::reply_t      reply;
        std::optional<node_handle_s> target{};
    };

    // Native config batch (not a transaction protocol). Explicit options validate atomically;
    // attached actions then run admission in order, each failing independently.
    // On an option error, no actions run; every supplied action receives a batch failure.
    // Each supplied action callback is settled exactly once, including rejection.
    // Work starts and callbacks run only after releasing the graph lock.
    nodes::set_options_result_s
    handle_control_batch(app_state_s*                        app,
                         std::span<const option_update_s>    updates,
                         std::vector<action_request_s>       actions,
                         const std::optional<origin_info_s>& origin = std::nullopt,
                         node_actions_s::clock_t::time_point now    = node_actions_s::clock_t::now());

    error_e handle_node_action(app_state_s*            app,
                               std::string_view        id,
                               std::string_view        name,
                               const nlohmann::json&   payload,
                               node_actions_s::reply_t reply);

    nlohmann::json get_node_status(std::string_view id) const;

    void add_adapter(std::unique_ptr<adapter_i>&& adapter);
    void clear_adapters();

    void tick_one_frame(app_state_s*, frame_scheduler_s&);
    void clear_nodes(app_state_s*);

    std::pair<std::shared_ptr<nodes::node_i>, error_e> create_node(std::string_view type);

  private:
    struct update_notice_s
    {
        bool corrected{};
        bool action_modified{};
    };
    using update_notices_t = utils::unordered_string_map_t<update_notice_s>;
    void emit_option_updates_locked(const update_notices_t& notices, const std::optional<origin_info_s>& origin);
    struct action_work_s
    {
        nodes::action_s                  action;
        nodes::action_context_s::start_t start;
        void                             run(app_state_s* app);
    };
    void admit_action_locked(app_state_s*                        app,
                             action_work_s&                      work,
                             update_notices_t&                   notices,
                             node_actions_s::clock_t::time_point now);
};

} // namespace miximus::core
