#pragma once
#include "decklink_iid.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace miximus::nodes::decklink {

template <typename T>
concept decklink_com_object = std::is_base_of_v<IUnknown, T>;

template <decklink_com_object T>
class decklink_ptr;

template <decklink_com_object T, decklink_com_object U>
decklink_ptr<T> query_decklink_interface(U* source);

template <decklink_com_object T>
class decklink_ptr
{
    template <decklink_com_object U>
    friend class decklink_ptr;

  public:
    constexpr decklink_ptr();
    constexpr decklink_ptr(std::nullptr_t);
    explicit decklink_ptr(T* ptr, bool take_ownership = true);
    decklink_ptr(const decklink_ptr<T>& other);
    decklink_ptr(decklink_ptr<T>&& other);

    ~decklink_ptr();

    decklink_ptr<T>& operator=(std::nullptr_t);
    decklink_ptr<T>& operator=(T* ptr);
    decklink_ptr<T>& operator=(const decklink_ptr<T>& other);
    decklink_ptr<T>& operator=(decklink_ptr<T>&& other);

    T*  get() const;
    T** releaseAndGetAddressOf();

    const T* operator->() const;
    T*       operator->();
    const T& operator*() const;
    T&       operator*();

    explicit operator bool() const;

    template <decklink_com_object U>
    decklink_ptr<U> query() const;

    auto operator<=>(const decklink_ptr<T>& other) const = default;

  private:
    void release();

    T* m_ptr;
};

template <decklink_com_object T>
constexpr decklink_ptr<T>::decklink_ptr()
    : m_ptr(nullptr)
{
}

template <decklink_com_object T>
constexpr decklink_ptr<T>::decklink_ptr(std::nullptr_t)
    : m_ptr(nullptr)
{
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(T* ptr, bool take_ownership)
    : m_ptr(ptr)
{
    if (take_ownership && m_ptr)
        m_ptr->AddRef();
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(const decklink_ptr<T>& other)
    : m_ptr(other.m_ptr)
{
    if (m_ptr)
        m_ptr->AddRef();
}

template <decklink_com_object T>
decklink_ptr<T>::decklink_ptr(decklink_ptr<T>&& other)
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
decklink_ptr<T>& decklink_ptr<T>::operator=(std::nullptr_t)
{
    release();
    m_ptr = nullptr;
    return *this;
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(T* ptr)
{
    if (ptr)
        ptr->AddRef();
    release();
    m_ptr = ptr;
    return *this;
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(const decklink_ptr<T>& other)
{
    return (*this = other.m_ptr);
}

template <decklink_com_object T>
decklink_ptr<T>& decklink_ptr<T>::operator=(decklink_ptr<T>&& other)
{
    release();
    m_ptr       = other.m_ptr;
    other.m_ptr = nullptr;
    return *this;
}

template <decklink_com_object T>
T* decklink_ptr<T>::get() const
{
    return m_ptr;
}

template <decklink_com_object T>
T** decklink_ptr<T>::releaseAndGetAddressOf()
{
    release();
    return &m_ptr;
}

template <decklink_com_object T>
const T* decklink_ptr<T>::operator->() const
{
    return m_ptr;
}

template <decklink_com_object T>
T* decklink_ptr<T>::operator->()
{
    return m_ptr;
}

template <decklink_com_object T>
const T& decklink_ptr<T>::operator*() const
{
    return *m_ptr;
}

template <decklink_com_object T>
T& decklink_ptr<T>::operator*()
{
    return *m_ptr;
}

template <decklink_com_object T>
decklink_ptr<T>::operator bool() const
{
    return m_ptr != nullptr;
}

template <decklink_com_object T>
template <decklink_com_object U>
decklink_ptr<U> decklink_ptr<T>::query() const
{
    return query_decklink_interface<U>(m_ptr);
}

template <decklink_com_object T>
void decklink_ptr<T>::release()
{
    if (m_ptr)
        m_ptr->Release();
}

template <decklink_com_object T, decklink_com_object U>
decklink_ptr<T> query_decklink_interface(U* source)
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
    return decklink_ptr(new T(args...), false);
}

} // namespace miximus::nodes::decklink
