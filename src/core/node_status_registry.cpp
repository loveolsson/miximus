#include "node_status_registry.hpp"

#include <nlohmann/json.hpp>

namespace miximus::core {

node_status_registry_s::writer_s::writer_s(node_status_registry_s*      registry,
                                           std::unique_lock<std::mutex> lock,
                                           const std::string*           node_id,
                                           nlohmann::json*              state)
    : registry_(registry)
    , lock_(std::move(lock))
    , node_id_(node_id)
    , state_(state)
{
}

void node_status_registry_s::writer_s::write(std::string_view key, nlohmann::json value)
{
    if (auto it = state_->find(key); it != state_->end() && *it == value) {
        return;
    }

    (*state_)[std::string(key)] = value;
    if (pending_ == nullptr) {
        pending_ = &registry_->pending_.try_emplace(*node_id_, nlohmann::json::object()).first->second;
    }
    (*pending_)[std::string(key)] = std::move(value);
}

void node_status_registry_s::write(std::string_view node_id, std::string_view key, nlohmann::json value)
{
    auto writer = write_node(node_id);
    writer.write(key, std::move(value));
}

node_status_registry_s::writer_s node_status_registry_s::write_node(std::string_view node_id)
{
    std::unique_lock lock(mutex_);
    auto [it, _] = states_.try_emplace(std::string(node_id), nlohmann::json::object());
    return {this, std::move(lock), &it->first, &it->second};
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
