#pragma once
#include <cstddef>
#include <ostream>

struct tab_s
{
    std::size_t level;
};

constexpr tab_s tab(std::size_t level) { return {level}; }

inline std::ostream& operator<<(std::ostream& stream, tab_s indentation)
{
    for (std::size_t i = 0; i < indentation.level; ++i) {
        stream << "    ";
    }

    return stream;
}
