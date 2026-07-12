#pragma once
#include "utils/transparent_string_hash.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
    struct pending_entry_s
    {
        std::string node_id;
        std::string key;
    };

    using state_map_t =
        std::unordered_map<std::string, nlohmann::json, utils::transparent_string_hash, std::equal_to<>>;

    mutable std::mutex           mutex_;
    state_map_t                  states_;
    std::vector<pending_entry_s> pending_;

  public:
    node_status_registry_s()  = default;
    ~node_status_registry_s() = default;

    node_status_registry_s(const node_status_registry_s&)            = delete;
    node_status_registry_s& operator=(const node_status_registry_s&) = delete;

    /**
     * Write a status entry for a node. Thread-safe, callable from any thread.
     * No-ops if the value is unchanged since the last write.
     */
    void write(std::string_view node_id, std::string_view key, nlohmann::json value);

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
