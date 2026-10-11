#include "environment.hpp"

#include <cstdlib>

namespace miximus::utils {

std::optional<std::string> environment_value(const char* name)
{
    // The application reads configuration from an immutable process environment.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const auto* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }

    return std::string(value);
}

} // namespace miximus::utils
