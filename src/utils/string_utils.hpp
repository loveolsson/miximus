#pragma once

#include <boost/locale/encoding_utf.hpp>

#include <string>
#include <string_view>

namespace miximus::utils {

inline std::string wide_to_utf8(std::wstring_view value)
{
    return boost::locale::conv::utf_to_utf<char>(value.data(), value.data() + value.size());
}

inline std::u32string utf8_to_utf32(std::string_view utf8_string)
{
    return boost::locale::conv::utf_to_utf<char32_t>(utf8_string.data(), utf8_string.data() + utf8_string.size());
}

} // namespace miximus::utils
