#pragma once
#include "utils/lookup.hpp"

namespace miximus {

enum class topic_e
{
    add_node,
    remove_node,
    add_connection,
    remove_connection,
    update_node,
    config,
};

constexpr auto topic_from_string = enum_from_string<topic_e>;

} // namespace miximus