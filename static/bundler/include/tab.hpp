#pragma once
#include <string>

inline auto tab(std::size_t len) { return std::string(len * 4, ' '); }

// #define TAB_0 ""
// #define TAB_1 "    "
// #define TAB_2 TAB_1 TAB_1
// #define TAB_3 TAB_2 TAB_1
// #define TAB_4 TAB_3 TAB_1
// #define TAB_5 TAB_4 TAB_1
// #define TAB_6 TAB_5 TAB_1
// #define TAB_7 TAB_6 TAB_1
// #define TAB_8 TAB_7 TAB_1
// #define TAB_9 TAB_8 TAB_1
// #define tab(n) TAB_##n