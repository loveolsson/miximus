#pragma once
#include "transparent_string_hash.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

namespace miximus::utils {

// These aliases retain the standard containers' native representation while
// enabling allocation-free lookup with compatible string and string-view keys.
template <typename Value>
using string_map_t = std::map<std::string, Value, std::less<>>;

template <typename Value>
using string_view_map_t = std::map<std::string_view, Value, std::less<>>;

template <typename Value>
using unordered_string_map_t = std::unordered_map<std::string, Value, transparent_string_hash, std::equal_to<>>;

template <typename Value>
using unordered_string_view_map_t =
    std::unordered_map<std::string_view, Value, transparent_string_hash, std::equal_to<>>;

} // namespace miximus::utils
