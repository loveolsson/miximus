#pragma once
#include "types/topic.hpp"

#include <set>
#include <string>

namespace miximus::web_server::detail {
struct websocket_connection
{
    int64_t           id;
    std::set<topic_e> topics;
};

} // namespace miximus::web_server::detail