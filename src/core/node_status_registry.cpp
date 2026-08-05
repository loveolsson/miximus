#include "node_status_registry.hpp"

#include <nlohmann/json.hpp>

namespace miximus::core {

void node_status_registry_s::write(std::string_view node_id, nlohmann::json status)
{
    std::scoped_lock lock(mutex_);
    auto [state, _]         = states_.try_emplace(std::string(node_id), nlohmann::json::object());
    nlohmann::json* pending = nullptr;

    for (auto& [name, value] : status.items()) {
        if (const auto current = state->second.find(name); current != state->second.end() && *current == value) {
            continue;
        }

        state->second[name] = value;
        if (pending == nullptr) {
            pending = &pending_.try_emplace(state->first, nlohmann::json::object()).first->second;
        }
        (*pending)[name] = std::move(value);
    }
}

void node_status_registry_s::remove_node(std::string_view node_id)
{
    std::scoped_lock lock(mutex_);
    if (const auto it = states_.find(node_id); it != states_.end()) {
        states_.erase(it);
    }
    if (const auto it = pending_.find(node_id); it != pending_.end()) {
        pending_.erase(it);
    }
}

std::vector<node_status_registry_s::status_update_s> node_status_registry_s::flush()
{
    std::scoped_lock lock(mutex_);

    std::vector<status_update_s> result;
    result.reserve(pending_.size());
    for (auto& [node_id, status] : pending_) {
        result.push_back({node_id, std::move(status)});
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
