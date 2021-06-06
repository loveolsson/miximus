#include "decklink.hpp"

#if _WIN32
static std::string ConvertWCSToMBS(const wchar_t* pstr, long wslen)
{
    int len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, NULL, 0, NULL, NULL);

    std::string dblstr(len, '\0');
    len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, &dblstr[0], len, NULL, NULL);

    return dblstr;
}

static std::string ConvertBSTRToMBS(BSTR bstr)
{
    int wslen = ::SysStringLen(bstr);
    return ConvertWCSToMBS((wchar_t*)bstr, wslen);
}

#endif

namespace miximus::nodes::decklink {
decklink_ptr<IDeckLinkIterator> get_device_iterator()
{
#if _WIN32
    IDeckLinkIterator* iteratror = NULL;

    auto res = CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (LPVOID*)&iteratror);

    if (SUCCEEDED(res)) {
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
        BSTR name;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(ConvertBSTRToMBS(name));
#else
        const char* name;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(name);
#endif
    }

    return res;
}

} // namespace miximus::nodes::decklink