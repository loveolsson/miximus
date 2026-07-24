#pragma once
#include "nodes/node_definition.hpp"

#include <vector>

namespace miximus::nodes::system {

std::vector<node_migration_s> settings_migrations();

} // namespace miximus::nodes::system
