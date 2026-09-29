#pragma once
#include "node_handle.hpp"

namespace miximus::core {
// Status uses the same instance identity as pending graph actions.
using node_status_handle_s = node_handle_s;
} // namespace miximus::core
