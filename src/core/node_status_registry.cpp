#include "node_status_registry.hpp"

#include <nlohmann/json.hpp>

namespace miximus::core {

void node_status_registry_s::write(std::string_view node_id, std::string_view key, nlohmann::json value)
{
    std::scoped_lock lock(mutex_);

    auto& node_state = states_[std::string(node_id)];

    if (auto it = node_state.find(key); it != node_state.end() && *it == value) {
        return; // unchanged
    }

    node_state[std::string(key)] = std::move(value);
    pending_.push_back({std::string(node_id), std::string(key)});
}

void node_status_registry_s::remove_node(std::string_view node_id)
{
    std::scoped_lock lock(mutex_);
    if (const auto it = states_.find(node_id); it != states_.end()) {
        states_.erase(it);
    }
}

std::vector<node_status_registry_s::status_update_s> node_status_registry_s::flush()
{
    std::scoped_lock lock(mutex_);

    std::vector<status_update_s>                                                             result;
    std::unordered_map<std::string, size_t, utils::transparent_string_hash, std::equal_to<>> update_indices;

    for (const auto& entry : pending_) {
        const auto node = states_.find(entry.node_id);
        if (node == states_.end()) {
            continue;
        }

        const auto value = node->second.find(entry.key);
        if (value == node->second.end()) {
            continue;
        }

        const auto [index, inserted] = update_indices.try_emplace(entry.node_id, result.size());
        if (inserted) {
            result.push_back({entry.node_id, nlohmann::json::object()});
        }

        result[index->second].status[entry.key] = *value;
    }

    pending_.clear();
    return result;
}

nlohmann::json node_status_registry_s::get(std::string_view node_id) const
{
    std::scoped_lock lock(mutex_);

    if (auto it = states_.find(node_id); it != states_.end()) {
        return it->second;
    }

    return nlohmann::json::object();
}

nlohmann::json node_status_registry_s::get_all() const
{
    std::scoped_lock lock(mutex_);
    return states_;
}

} // namespace miximus::core
