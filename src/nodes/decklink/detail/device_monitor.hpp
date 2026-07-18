#pragma once
#include "nodes/decklink/registry.hpp"

#include <memory>

struct IDeckLink;

namespace miximus::nodes::decklink::detail {

class device_monitor_s
{
    class impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    explicit device_monitor_s(IDeckLink* device);
    ~device_monitor_s();

    device_monitor_s(const device_monitor_s&)            = delete;
    device_monitor_s& operator=(const device_monitor_s&) = delete;

    void                                   poll_statistics();
    std::shared_ptr<const device_status_s> status() const;
};

} // namespace miximus::nodes::decklink::detail
