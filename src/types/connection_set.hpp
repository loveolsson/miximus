#pragma once
#include "types/connection.hpp"

#include <unordered_set>

namespace miximus::nodes {

typedef std::unordered_set<connection, connection_hash> con_set_t;

} // namespace miximus::nodes