#pragma once
#include "types/topic.hpp"
#include "utils/lookup.hpp"

#include <cstdint>

namespace miximus::web_server::detail {
struct websocket_connection
{
    int64_t                     id;
    enum_array_t<topic_e, bool> topics;

    bool has_subscription(topic_e t) const noexcept { return topics[t]; }
    void set_subscription(topic_e t, bool value) noexcept { topics[t] = value; }
};

} // namespace miximus::web_server::detail
