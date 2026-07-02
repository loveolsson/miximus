#pragma once
#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace miximus::core {

class node_status_registry_s
{
    struct pending_entry_s
    {
        std::string node_id;
        std::string key;
    };

    mutable std::mutex                              mutex_;
    std::unordered_map<std::string, nlohmann::json> states_;
    std::vector<pending_entry_s>                    pending_;

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
     * Drain pending changes and return the IDs of nodes whose status changed
     * since the last flush. Called once per tick from tick_one_frame.
     */
    std::vector<std::string> flush();

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
