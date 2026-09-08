#pragma once
#include "types/node_status_json.hpp"
#include "utils/string_map.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::core {

class node_status_registry_s
{
  public:
    struct status_update_s
    {
        std::string    node_id;
        nlohmann::json status;
    };

  private:
    using state_map_t = utils::unordered_string_map_t<nlohmann::json>;

    mutable std::mutex mutex_;
    state_map_t        states_;
    state_map_t        pending_;

    void write_json(std::string_view node_id, nlohmann::json status);

  public:
    node_status_registry_s()  = default;
    ~node_status_registry_s() = default;

    node_status_registry_s(const node_status_registry_s&)            = delete;
    node_status_registry_s& operator=(const node_status_registry_s&) = delete;

    /**
     * Write a registered status object for a node. Thread-safe and callable
     * from any thread; unchanged fields are filtered out.
     */
    template <status::registered_contract T>
    void write(std::string_view node_id, const T& value)
    {
        write_json(node_id, value);
    }

    /**
     * Remove all status entries for a node. Called when a node is destroyed.
     */
    void remove_node(std::string_view node_id);

    /**
     * Drain pending changes and return per-node status deltas. Multiple writes
     * to a node during one tick are merged into a single update.
     */
    std::vector<status_update_s> flush();

    /**
     * Return the current status object for a single node (for pull queries).
     */
    nlohmann::json get(std::string_view node_id) const;

    /**
     * Return a map of all node statuses (for inclusion in get_config responses).
     */
    nlohmann::json get_all() const;
};

} // namespace miximus::core
