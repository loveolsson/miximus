#include <cstddef>

namespace miximus {

template <typename T, size_t N>
constexpr size_t countof(T (&arr)[N])
{
    return std::extent<T[N]>::value;
}

} // namespace miximus
