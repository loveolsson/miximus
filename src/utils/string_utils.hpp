#pragma once

#include <cstdint>
#include <string>

namespace miximus::utils {

inline std::u32string utf8_to_utf32(const std::string& utf8_string)
{
    std::u32string result;
    result.reserve(utf8_string.size());

    const auto* p   = reinterpret_cast<const uint8_t*>(utf8_string.data());
    const auto* end = p + utf8_string.size();

    while (p < end) {
        char32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (*p++ & 0x1F) << 6;
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F);
            }
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (*p++ & 0x0F) << 12;
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F) << 6;
            }
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F);
            }
        } else if ((*p & 0xF8) == 0xF0) {
            cp = (*p++ & 0x07) << 18;
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F) << 12;
            }
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F) << 6;
            }
            if (p < end && (*p & 0xC0) == 0x80) {
                cp |= (*p++ & 0x3F);
            }
        } else {
            ++p; // skip invalid byte
            continue;
        }
        result.push_back(cp);
    }

    return result;
}

} // namespace miximus::utils
