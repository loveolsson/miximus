#pragma once
#include <mapbox/eternal.hpp>

namespace miximus::utils {

template <typename Key, typename Value, size_t N>
constexpr auto const_map_t(const std::pair<const Key, const Value> (&items)[N])
{
    return mapbox::eternal::map<Key, Value>(items);
}

} // namespace miximus::utils