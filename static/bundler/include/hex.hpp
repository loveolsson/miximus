#pragma once
#include <format>
#include <string>

inline std::string fmt_u8(uint8_t c) { return std::format("{:#04x}", c); }
