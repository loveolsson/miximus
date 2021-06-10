#pragma once
#include "messages/types.hpp"

#include <set>
#include <string>

namespace miximus::web_server::detail {
struct websocket_connection
{
    int64_t                    id;
    std::set<message::topic_e> topics;
};

} // namespace miximus::web_server::detail