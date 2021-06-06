#pragma once

namespace miximus::nodes::decklink {

#define IID_FROM_TYPE(x) IID_##x

#define QUERY_INTERFACE (D, T) D.query_interface(D, IID_FROM_TYPE(T))

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
        }
    }

    operator bool() { return ptr_ != nullptr; }

    T& operator*() { return *this->ptr; }

    T* operator->() { return this->ptr; }

    static decklink_ptr<T> query_interface(REFIID iid)
    {
        if (!ptr_) {
            return nullptr;
        }

        T* t;

        if (SUCCEEDED(ptr_->QueryInterface(iid, (void**)&t))) {
            return t;
        }

        return nullptr;
    }
};
} // namespace miximus::nodes::decklink