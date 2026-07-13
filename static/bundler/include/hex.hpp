#pragma once
#include <cstdint>
#include <ostream>

struct hex_u8_s
{
    uint8_t value;
};

constexpr hex_u8_s hex_u8(uint8_t value) { return {value}; }

inline std::ostream& operator<<(std::ostream& stream, hex_u8_s hex)
{
    constexpr char digits[] = "0123456789abcdef";
    const char     buffer[] = {'0', 'x', digits[hex.value >> 4], digits[hex.value & 0x0f]};
    return stream.write(buffer, sizeof(buffer));
}
