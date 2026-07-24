#pragma once
#include <boost/url/segments_view.hpp>

#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace miximus::web_server::detail {

inline bool path_starts_with(boost::urls::segments_view path, std::initializer_list<std::string_view> prefix)
{
    if (path.size() < prefix.size()) {
        return false;
    }

    auto segment = path.begin();
    for (const auto expected : prefix) {
        if (*segment != expected) {
            return false;
        }
        ++segment;
    }
    return true;
}

inline bool path_matches(boost::urls::segments_view path, std::initializer_list<std::string_view> expected)
{
    return path.size() == expected.size() && path_starts_with(path, expected);
}

inline boost::urls::segments_view consume_segments(boost::urls::segments_view path, size_t count) noexcept
{
    auto segment = path.begin();
    while (count > 0 && segment != path.end()) {
        ++segment;
        --count;
    }
    return boost::urls::segments_view(segment, path.end());
}

} // namespace miximus::web_server::detail
