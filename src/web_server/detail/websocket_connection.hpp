#pragma once
#include "types/topic.hpp"

#include <array>

namespace miximus::web_server::detail {
struct websocket_connection
{
    int64_t                                 id;
    std::array<bool, enum_count<topic_e>()> topics;

    bool has_subscription(topic_e t) const noexcept
    {
        return topics[enum_index(t)];
    } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    void set_subscription(topic_e t, bool value) noexcept
    {
        topics[enum_index(t)] = value;
    } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
};

} // namespace miximus::web_server::detail