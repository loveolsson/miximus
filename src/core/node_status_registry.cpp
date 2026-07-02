#include "node_status_registry.hpp"

#include <nlohmann/json.hpp>

#include <unordered_set>

namespace miximus::core {

void node_status_registry_s::write(std::string_view node_id, std::string_view key, nlohmann::json value)
{
    std::lock_guard lock(mutex_);

    auto& node_state = states_[std::string(node_id)];

    if (auto it = node_state.find(key); it != node_state.end() && *it == value) {
        return; // unchanged
    }

    node_state[std::string(key)] = value;
    pending_.push_back({std::string(node_id), std::string(key)});
}

void node_status_registry_s::remove_node(std::string_view node_id)
{
    std::lock_guard lock(mutex_);
    states_.erase(std::string(node_id));
}

std::vector<std::string> node_status_registry_s::flush()
{
    std::vector<pending_entry_s> changed;
    {
        std::lock_guard lock(mutex_);
        changed.swap(pending_);
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string>        result;

    for (auto& entry : changed) {
        if (seen.emplace(entry.node_id).second) {
            result.push_back(std::move(entry.node_id));
        }
    }

    return result;
}

nlohmann::json node_status_registry_s::get(std::string_view node_id) const
{
    std::lock_guard lock(mutex_);

    if (auto it = states_.find(std::string(node_id)); it != states_.end()) {
        return it->second;
    }

    return nlohmann::json::object();
}

nlohmann::json node_status_registry_s::get_all() const
{
    std::lock_guard lock(mutex_);
    return states_;
}

} // namespace miximus::core
