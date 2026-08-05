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
    using state_map_t =
        std::unordered_map<std::string, nlohmann::json, utils::transparent_string_hash, std::equal_to<>>;

    mutable std::mutex mutex_;
    state_map_t        states_;
    state_map_t        pending_;

  public:
    node_status_registry_s()  = default;
    ~node_status_registry_s() = default;

    node_status_registry_s(const node_status_registry_s&)            = delete;
    node_status_registry_s& operator=(const node_status_registry_s&) = delete;

    /**
     * Write a status object for a node. Typed status structs convert to JSON
     * through ADL before this function is entered. Thread-safe and callable
     * from any thread; unchanged fields are filtered out.
     */
    void write(std::string_view node_id, nlohmann::json status);

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
