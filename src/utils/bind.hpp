#pragma once

namespace miximus::utils {

template <typename R, typename T, typename... Args>
auto bind(R (T::*f)(Args...), T* p)
{
    return [p, f](Args&&... args) -> R { return (p->*f)(std::forward<Args>(args)...); };
};

} // namespace miximus::utils