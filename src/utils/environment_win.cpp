#include "environment.hpp"

#include <cstdlib>
#include <memory>
#include <system_error>

namespace miximus::utils {

std::optional<std::string> environment_value(const char* name)
{
    char*                                             value = nullptr;
    size_t                                            length{};
    const auto                                        result = _dupenv_s(&value, &length, name);
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "Read environment variable");
    }

    if (!owned) {
        return std::nullopt;
    }

    return std::string(owned.get(), length - 1);
}

} // namespace miximus::utils
