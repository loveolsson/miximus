#pragma once

#include <optional>
#include <string>

namespace miximus::utils {

// Returns an owned value. Callers must not concurrently mutate the process
// environment; startup options and test configuration are read-only.
std::optional<std::string> environment_value(const char* name);

} // namespace miximus::utils
