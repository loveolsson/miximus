#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace miximus::core {
class node_status_registry_s;

// Identity of one admitted instance. Copies retain metadata, never the node.
// Retirement controls config-side publication; frame consumers compare identity
// with their stable snapshot instead of consulting the live active flag.
class node_handle_s
{
    struct identity_s
    {
        explicit identity_s(std::string_view node_id)
            : id(node_id)
        {
        }
        const std::string id;
        std::atomic<bool> active{true};
    };
    std::shared_ptr<identity_s> identity_;
    friend class node_status_registry_s;

  public:
    node_handle_s() = default;
    explicit node_handle_s(std::string_view id)
        : identity_(std::make_shared<identity_s>(id))
    {
    }
    std::string_view id() const noexcept { return identity_ ? std::string_view(identity_->id) : std::string_view{}; }
    bool             active() const noexcept { return identity_ && identity_->active.load(); }
    void             retire() const noexcept
    {
        if (identity_) {
            identity_->active.store(false);
        }
    }
    bool operator==(const node_handle_s& other) const noexcept = default;
};
} // namespace miximus::core
