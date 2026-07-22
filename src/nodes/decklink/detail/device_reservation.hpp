#pragma once

#include <memory>
#include <mutex>
#include <unordered_set>

namespace miximus::nodes::decklink::detail {

// Prevents multiple asynchronously starting nodes from claiming the same SDK
// interface. Input and output reservations are independent template instances.
template <typename Device>
class device_reservation_s
{
    Device* device_;

    static auto& mutex()
    {
        static std::mutex value;
        return value;
    }

    static auto& devices()
    {
        static std::unordered_set<Device*> value;
        return value;
    }

    explicit device_reservation_s(Device* device)
        : device_(device)
    {
    }

  public:
    ~device_reservation_s()
    {
        const std::scoped_lock lock(mutex());
        devices().erase(device_);
    }

    device_reservation_s(const device_reservation_s&)            = delete;
    device_reservation_s& operator=(const device_reservation_s&) = delete;
    device_reservation_s(device_reservation_s&&)                 = delete;
    device_reservation_s& operator=(device_reservation_s&&)      = delete;

    static auto acquire(Device* device) -> std::shared_ptr<device_reservation_s>
    {
        const std::scoped_lock lock(mutex());
        if (device == nullptr || !devices().emplace(device).second) {
            return {};
        }
        return std::shared_ptr<device_reservation_s>(new device_reservation_s(device));
    }
};

} // namespace miximus::nodes::decklink::detail
