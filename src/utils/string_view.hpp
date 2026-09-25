#pragma once
#include <algorithm>
#include <string_view>

namespace miximus::utils {

constexpr auto ASCII_WHITESPACES = " \n\r\t\f\v";

/**
 * Trim leading ASCII whitespaces from a string_view.
 * Returns a new more narrow string_view window.
 */
[[nodiscard]] constexpr std::string_view ltrim_view(std::string_view str) noexcept
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
[[nodiscard]] constexpr std::string_view rtrim_view(std::string_view str) noexcept
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
[[nodiscard]] constexpr std::string_view trim_view(std::string_view str) noexcept
{
    return rtrim_view(ltrim_view(str));
}

/**
 * Convert an ASCII character to lower case
 */
[[nodiscard]] constexpr char ascii_to_lower(char c) noexcept
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
[[nodiscard]] constexpr bool ascii_ieq_view(std::string_view a, const char (&b)[S]) noexcept
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
[[nodiscard]] constexpr bool ascii_ieq_view(std::string_view a, std::string_view b) noexcept
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
/**
 * Case insensitive substring search for an ASCII string_view.
 * An empty token matches any text. Bytes outside ASCII are compared unchanged.
 */
[[nodiscard]] constexpr bool ascii_icontains_view(std::string_view text, std::string_view token) noexcept
{
    if (token.empty()) {
        return true;
    }
    return std::search(text.begin(), text.end(), token.begin(), token.end(), [](char a, char b) {
               return ascii_to_lower(a) == ascii_to_lower(b);
           }) != text.end();
}

/**
 * Case insensitive substring search for a static ASCII token.
 * Preserve the token length without scanning for a null terminator.
 */
template <size_t S>
[[nodiscard]] constexpr bool ascii_icontains_view(std::string_view text, const char (&token)[S]) noexcept
{
    return ascii_icontains_view(text, std::string_view(token, S - 1));
}
} // namespace miximus::utils
