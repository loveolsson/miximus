#pragma once

#include <string>
#include <locale>
#include <codecvt>

namespace miximus::utils {

/**
 * @brief Convert UTF-8 encoded string to UTF-32 encoded string
 * 
 * This utility function converts a UTF-8 string to UTF-32 for proper
 * Unicode handling in text rendering and processing systems.
 * 
 * @param utf8_string The input UTF-8 encoded string
 * @return std::u32string The UTF-32 encoded string
 */
inline std::u32string utf8_to_utf32(const std::string& utf8_string)
{
    struct destructible_codecvt : public std::codecvt<char32_t, char, std::mbstate_t>
    {
    };

    std::wstring_convert<destructible_codecvt, char32_t> utf32_converter;
    return utf32_converter.from_bytes(utf8_string);
}

} // namespace miximus::utils
