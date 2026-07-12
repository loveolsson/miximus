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
    font_registry,
    config,
    node_status,
};

constexpr std::optional<topic_e> topic_from_string(std::string_view value) { return enum_from_string<topic_e>(value); }

} // namespace miximus
