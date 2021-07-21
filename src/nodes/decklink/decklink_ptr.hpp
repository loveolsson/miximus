#pragma once
#ifdef _WIN32
#include <unknwn.h>

#include <DeckLinkAPI_i.h>
#else
#include <DeckLinkAPI.h>
#endif

namespace miximus::nodes::decklink {

#define IID_FROM_TYPE(x) IID_##x
#define QUERY_INTERFACE(D, T) D.query_interface<T>(IID_FROM_TYPE(T))

template <typename T>
class decklink_ptr
{
    T* ptr_;

  public:
    decklink_ptr()
        : ptr_(nullptr)
    {
    }

    decklink_ptr(T* ptr)
        : ptr_(ptr)
    {
    }

    decklink_ptr(decklink_ptr&& o)
        : ptr_(o.ptr_)
    {
        o.ptr_ = nullptr;
    }

    decklink_ptr(const decklink_ptr& o)
        : ptr_(o.ptr_)
    {
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    static decklink_ptr make_owner(T* ptr)
    {
        if (ptr) {
            ptr->AddRef();
        }

        return ptr;
    }

    ~decklink_ptr()
    {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    decklink_ptr& operator=(decklink_ptr&& o)
    {
        ~decklink_ptr();
        ptr_   = o.ptr_;
        o.ptr_ = nullptr;
    }

    decklink_ptr& operator=(const decklink_ptr& o)
    {
        ~decklink_ptr();
        ptr_ = o.ptr_;
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    decklink_ptr& operator=(T* ptr)
    {
        ~decklink_ptr();
        ptr_ = ptr;
    }

    operator bool() const { return ptr_ != nullptr; }

    bool operator==(const decklink_ptr& o) const { return ptr_ == o.ptr_; }
    bool operator!=(const decklink_ptr& o) const { return ptr_ != o.ptr_; }
    bool operator==(const T* o) const { return ptr_ == o; }
    bool operator!=(const T* o) const { return ptr_ != o; }

    T& operator*() { return *ptr_; }
    T* operator->() { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }

    template <typename R>
    decklink_ptr<R> query_interface(REFIID iid) const
    {
        if (!ptr_) {
            return nullptr;
        }

        R* t = nullptr;

        if (SUCCEEDED(ptr_->QueryInterface(iid, (void**)&t))) {
            return t;
        }

        return nullptr;
    }
};
} // namespace miximus::nodes::decklink