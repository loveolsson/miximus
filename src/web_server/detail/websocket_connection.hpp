#pragma once
#include "types/topic.hpp"

#include <array>

namespace miximus::web_server::detail {
struct websocket_connection
{
    int64_t                                 id;
    std::array<bool, enum_count<topic_e>()> topics;
};

} // namespace miximus::web_server::detail