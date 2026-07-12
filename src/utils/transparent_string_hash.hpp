#pragma once

#include <functional>
#include <string_view>

namespace miximus::utils {

struct transparent_string_hash
{
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

} // namespace miximus::utils
