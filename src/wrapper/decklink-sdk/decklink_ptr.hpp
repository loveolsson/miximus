#pragma once
#include "decklink_iid.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace miximus::decklink_sdk {

template <typename T>
concept decklink_com_object = std::is_base_of_v<IUnknown, T>;

template <decklink_com_object T>
class decklink_ptr;

template <decklink_com_object T, decklink_com_object U>
decklink_ptr<T> query_decklink_interface(U* source) noexcept;

template <decklink_com_object T>
class decklink_ptr
{
    template <decklink_com_object U>
    friend class decklink_ptr;

  public:
    constexpr decklink_ptr() noexcept;
    constexpr decklink_ptr(std::nullptr_t) noexcept;
    explicit decklink_ptr(T* ptr, bool take_ownership = true) noexcept;
    decklink_ptr(const decklink_ptr<T>& other) noexcept;
    decklink_ptr(decklink_ptr<T>&& other) noexcept;

    ~decklink_ptr();

    decklink_ptr<T>& operator=(std::nullptr_t) noexcept;
    decklink_ptr<T>& operator=(T* ptr) noexcept;
    decklink_ptr<T>& operator=(const decklink_ptr<T>& other) noexcept;
    decklink_ptr<T>& operator=(decklink_ptr<T>&& other) noexcept;

    T*  get() const noexcept;
    T** releaseAndGetAddressOf() noexcept;

    const T* operator->() const noexcept;
    T*       operator->() noexcept;
    const T& operator*() const noexcept;
    T&       operator*() noexcept;

    explicit operator bool() const noexcept;

    template <decklink_com_object U>
    decklink_ptr<U> query() const noexcept;

    auto operator<=>(const decklink_ptr<T>& other) const = default;

  private:
    void release() noexcept;

    T* m_ptr;
};

template <decklink_com_object T>
constexpr decklink_ptr<T>::decklink_ptr() noexcept
    : m_ptr(nullptr)
{
}

template <decklink_com_object T>
constexpr decklink_ptr<T>::decklink_ptr(std::nullptr_t) noexcept
    : m_ptr(nullptr)
{
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(T* ptr, bool take_ownership) noexcept
    : m_ptr(ptr)
{
    if (take_ownership && m_ptr)
        m_ptr->AddRef();
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(const decklink_ptr<T>& other) noexcept
    : m_ptr(other.m_ptr)
{
    if (m_ptr)
        m_ptr->AddRef();
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(decklink_ptr<T>&& other) noexcept
    : m_ptr(other.m_ptr)
{
    other.m_ptr = nullptr;
}

template <decklink_com_object T>
decklink_ptr<T>::~decklink_ptr()
{
    release();
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(std::nullptr_t) noexcept
{
    release();
    m_ptr = nullptr;
    return *this;
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(T* ptr) noexcept
{
    if (ptr)
        ptr->AddRef();
    release();
    m_ptr = ptr;
    return *this;
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(const decklink_ptr<T>& other) noexcept
{
    return (*this = other.m_ptr);
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(decklink_ptr<T>&& other) noexcept
{
    release();
    m_ptr       = other.m_ptr;
    other.m_ptr = nullptr;
    return *this;
}

template <decklink_com_object T>
T* decklink_ptr<T>::get() const noexcept
{
    return m_ptr;
}

template <decklink_com_object T>
T** decklink_ptr<T>::releaseAndGetAddressOf() noexcept
{
    release();
    return &m_ptr;
}

template <decklink_com_object T>
const T* decklink_ptr<T>::operator->() const noexcept
{
    return m_ptr;
}

template <decklink_com_object T>
T* decklink_ptr<T>::operator->() noexcept
{
    return m_ptr;
}

template <decklink_com_object T>
const T& decklink_ptr<T>::operator*() const noexcept
{
    return *m_ptr;
}

template <decklink_com_object T>
T& decklink_ptr<T>::operator*() noexcept
{
    return *m_ptr;
}

template <decklink_com_object T>
decklink_ptr<T>::operator bool() const noexcept
{
    return m_ptr != nullptr;
}

template <decklink_com_object T>
template <decklink_com_object U>
decklink_ptr<U> decklink_ptr<T>::query() const noexcept
{
    return query_decklink_interface<U>(m_ptr);
}

template <decklink_com_object T>
void decklink_ptr<T>::release() noexcept
{
    if (auto* ptr = std::exchange(m_ptr, nullptr); ptr != nullptr)
        ptr->Release();
}

template <decklink_com_object T, decklink_com_object U>
decklink_ptr<T> query_decklink_interface(U* source) noexcept
{
    if (source == nullptr) {
        return {};
    }

    T* result = nullptr;
    if (FAILED(source->QueryInterface(decklink_iid<T>(), reinterpret_cast<void**>(&result)))) {
        return {};
    }

    return decklink_ptr<T>(result, false);
}

template <decklink_com_object T, class... Args>
decklink_ptr<T> make_decklink_ptr(Args&&... args)
{
    return decklink_ptr(new T(std::forward<Args>(args)...), false);
}

static_assert(std::is_nothrow_move_constructible_v<decklink_ptr<IUnknown>>);
static_assert(std::is_nothrow_move_assignable_v<decklink_ptr<IUnknown>>);

} // namespace miximus::decklink_sdk
