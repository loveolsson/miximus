#pragma once

#include <boost/locale/utf.hpp>

#include <string>
#include <string_view>

namespace miximus::utils {

inline std::u32string utf8_to_utf32(std::string_view utf8_string)
{
    std::u32string result;
    result.reserve(utf8_string.size());

    using traits = boost::locale::utf::utf_traits<char>;
    auto p       = utf8_string.cbegin();
    auto end     = utf8_string.cend();

    while (p != end) {
        auto cp = traits::decode(p, end);
        if (cp != boost::locale::utf::illegal && cp != boost::locale::utf::incomplete) {
            result.push_back(static_cast<char32_t>(cp));
        }
    }

    return result;
}

} // namespace miximus::utils
