#pragma once
#include "decklink_inc.hpp"

#include <cstddef>
#include <utility>

namespace miximus::nodes::decklink {

template <typename T>
class decklink_ptr
{
    template <typename U>
    friend class decklink_ptr;

  public:
    constexpr decklink_ptr();
    constexpr decklink_ptr(std::nullptr_t);
    explicit decklink_ptr(T* ptr);
    decklink_ptr(const decklink_ptr<T>& other);
    decklink_ptr(decklink_ptr<T>&& other);

    template <typename U>
    decklink_ptr(REFIID iid, decklink_ptr<U> other);

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

    bool operator==(const decklink_ptr<T>& other) const;
    bool operator!=(const decklink_ptr<T>& other) const;
    bool operator<(const decklink_ptr<T>& other) const;

  private:
    void release();

    T* m_ptr;
};

template <typename T>
constexpr decklink_ptr<T>::decklink_ptr()
    : m_ptr(nullptr)
{
}

template <typename T>
constexpr decklink_ptr<T>::decklink_ptr(std::nullptr_t)
    : m_ptr(nullptr)
{
}

template <typename T>
decklink_ptr<T>::decklink_ptr(T* ptr)
    : m_ptr(ptr)
{
    if (m_ptr)
        m_ptr->AddRef();
}

template <typename T>
decklink_ptr<T>::decklink_ptr(const decklink_ptr<T>& other)
    : m_ptr(other.m_ptr)
{
    if (m_ptr)
        m_ptr->AddRef();
}

template <typename T>
decklink_ptr<T>::decklink_ptr(decklink_ptr<T>&& other)
    : m_ptr(other.m_ptr)
{
    other.m_ptr = nullptr;
}

template <typename T>
template <typename U>
decklink_ptr<T>::decklink_ptr(REFIID iid, decklink_ptr<U> other)
{
    if (other.m_ptr) {
        if (other.m_ptr->QueryInterface(iid, (void**)&m_ptr) != S_OK)
            m_ptr = nullptr;
    } else {
        m_ptr = nullptr;
    }
}

template <typename T>
decklink_ptr<T>::~decklink_ptr()
{
    release();
}

template <typename T>
decklink_ptr<T>& decklink_ptr<T>::operator=(std::nullptr_t)
{
    release();
    m_ptr = nullptr;
    return *this;
}

template <typename T>
decklink_ptr<T>& decklink_ptr<T>::operator=(T* ptr)
{
    if (ptr)
        ptr->AddRef();
    release();
    m_ptr = ptr;
    return *this;
}

template <typename T>
decklink_ptr<T>& decklink_ptr<T>::operator=(const decklink_ptr<T>& other)
{
    return (*this = other.m_ptr);
}

template <typename T>
decklink_ptr<T>& decklink_ptr<T>::operator=(decklink_ptr<T>&& other)
{
    release();
    m_ptr       = other.m_ptr;
    other.m_ptr = nullptr;
    return *this;
}

template <typename T>
T* decklink_ptr<T>::get() const
{
    return m_ptr;
}

template <typename T>
T** decklink_ptr<T>::releaseAndGetAddressOf()
{
    release();
    return &m_ptr;
}

template <typename T>
const T* decklink_ptr<T>::operator->() const
{
    return m_ptr;
}

template <typename T>
T* decklink_ptr<T>::operator->()
{
    return m_ptr;
}

template <typename T>
const T& decklink_ptr<T>::operator*() const
{
    return *m_ptr;
}

template <typename T>
T& decklink_ptr<T>::operator*()
{
    return *m_ptr;
}

template <typename T>
decklink_ptr<T>::operator bool() const
{
    return m_ptr != nullptr;
}

template <typename T>
void decklink_ptr<T>::release()
{
    if (m_ptr)
        m_ptr->Release();
}

template <typename T>
bool decklink_ptr<T>::operator==(const decklink_ptr<T>& other) const
{
    return m_ptr == other.m_ptr;
}

template <typename T>
bool decklink_ptr<T>::operator!=(const decklink_ptr<T>& other) const
{
    return m_ptr != other.m_ptr;
}

template <typename T>
bool decklink_ptr<T>::operator<(const decklink_ptr<T>& other) const
{
    return m_ptr < other.m_ptr;
}

template <class T, class... Args>
decklink_ptr<T> make_decklink_ptr(Args&&... args)
{
    auto*           t = new T(args...);
    decklink_ptr<T> temp(t);

    // decklink_ptr takes ownership of reference count, so release reference count added by raw pointer constructor
    t->Release();

    return std::move(temp);
}

} // namespace miximus::nodes::decklink