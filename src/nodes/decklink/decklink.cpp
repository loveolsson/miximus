#include "decklink.hpp"

namespace miximus::nodes::decklink {

#if _WIN32
string bstr_to_str(BSTR source)
{
    _bstr_t wrapped_bstr = _bstr_t(source);
    int     length       = wrapped_bstr.length();
    char*   char_array   = new char[length];
    strcpy_s(char_array, length + 1, wrapped_bstr);
    return char_array;
}
#endif

decklink_ptr<IDeckLinkIterator> get_device_iterator()
{
#if _WIN32
    IDeckLinkIterator* iteratror = NULL;

    auto res = CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void*)&iteratror);

    if (SUCCESS(res)) {
        return iteratror;
    }

    return nullptr;
#else
    return CreateDeckLinkIteratorInstance();
#endif
}

decklink_ptr<IDeckLink> get_device_by_index(int i)
{
    int  c        = 0;
    auto iterator = get_device_iterator();

    if (!iterator) {
        return nullptr;
    }

    IDeckLink* ptr;
    while (iterator->Next(&ptr) == S_OK) {
        auto dl_ptr = decklink_ptr(ptr);

        if (c++ == i) {
            return dl_ptr;
        }
    }

    return nullptr;
}

std::vector<std::string> get_device_names()
{
    std::vector<std::string> res;

    auto iterator = get_device_iterator();

    if (!iterator) {
        return res;
    }

    IDeckLink* ptr;
    while (iterator->Next(&ptr) == S_OK) {
        auto dl_ptr = decklink_ptr(ptr);

#if _WIN32
        BSTR str;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(bstr_to_str(name));
#else
        const char* name;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(name);
#endif
    }

    return res;
}

} // namespace miximus::nodes::decklink