#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace miximus::core {

class node_status_registry_s;

// An identity belongs to one admitted node instance, not merely its reusable ID.
class node_status_handle_s
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
    node_status_handle_s() = default;
    explicit node_status_handle_s(std::string_view id)
        : identity_(std::make_shared<identity_s>(id))
    {
    }
    bool active() const noexcept { return identity_ && identity_->active.load(); }
    void retire() const noexcept
    {
        if (identity_) {
            identity_->active.store(false);
        }
    }
};

} // namespace miximus::core
