#pragma once
#include <fmt/format.h>

inline std::string fmt_u8(uint8_t c) { return fmt::format("{:#04x}", c); }
