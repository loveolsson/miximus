#pragma once
#include <string_view>

namespace miximus::utils {

constexpr auto ASCII_WHITESPACES = " \n\r\t\f\v";

/**
 * Trim leading ASCII whitespaces from a string_view.
 * Returns a new more narrow string_view window.
 */
constexpr std::string_view ltrim_view(std::string_view str)
{
    auto index = str.find_first_not_of(ASCII_WHITESPACES);

    if (index != std::string_view::npos) {
        return {str.data() + index, str.size() - index};
    }

    return {};
}

/**
 * Trim trailing ASCII whitespaces from a string_view.
 * Returns a new more narrow string_view window.
 */
constexpr std::string_view rtrim_view(std::string_view str)
{
    auto index = str.find_last_not_of(ASCII_WHITESPACES);

    if (index != std::string_view::npos) {
        return {str.data(), index + 1};
    }

    return {};
}

/**
 * Trim leading and trailing ASCII whitespaces from a string_view.
 * Returns a new more narrow string_view window.
 */
constexpr std::string_view trim_view(std::string_view str) { return rtrim_view(ltrim_view(str)); }

/**
 * Convert an ASCII character to lower case
 */
constexpr char ascii_to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        c += 32;
    return c;
}

/**
 * Case insensitive compare an ASCII string_view to a static token.
 * The token being static allows the compiler to unroll the loop properly.
 */
template <size_t S>
constexpr bool ascii_ieq_view(std::string_view a, const char (&b)[S])
{
    auto len = S - 1;

    if (a.size() != len) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (ascii_to_lower(a[i]) != ascii_to_lower(b[i])) {
            return false;
        }
    }

    return true;
}

/**
 * Case insensitive compare two ASCII string_views.
 */
constexpr bool ascii_ieq_view(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_to_lower(a[i]) != ascii_to_lower(b[i])) {
            return false;
        }
    }

    return true;
}
} // namespace miximus::utils