#include "decklink.hpp"
#include "logger/logger.hpp"

#if _WIN32
static std::string wcs_tp_mbs(const wchar_t* pstr, long wslen)
{
    int len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, NULL, 0, NULL, NULL);

    std::string dblstr(len, '\0');
    len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, &dblstr[0], len, NULL, NULL);

    return dblstr;
}

static std::string bstr_to_mbs(BSTR bstr)
{
    int wslen = ::SysStringLen(bstr);
    return wcs_tp_mbs((wchar_t*)bstr, wslen);
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

    IDeckLink* ptr = nullptr;
    while (iterator->Next(&ptr) == S_OK) {
        decklink_ptr dl_ptr(ptr);

        if (c++ == i) {
            return dl_ptr;
        }
    }

    return nullptr;
}

static std::vector<std::string> get_device_names_impl()
{
    std::vector<std::string> res;

    auto iterator = get_device_iterator();

    if (!iterator) {
        return res;
    }

    IDeckLink* ptr = nullptr;
    while (iterator->Next(&ptr) == S_OK) {
        decklink_ptr dl_ptr(ptr);

#if _WIN32
        BSTR name;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(bstr_to_mbs(name));
#else
        const char* name = nullptr;
        dl_ptr->GetDisplayName(&name);
        res.emplace_back(name);
#endif
    }

    return res;
}

std::vector<std::string> get_device_names()
{
    static auto names = get_device_names_impl();
    return names;
}

void log_device_names()
{
    auto log   = spdlog::get("app");
    auto names = nodes::decklink::get_device_names();
    log->info("Found {} DeckLink device(s)", names.size());
    for (auto& name : names) {
        log->info(" -- \"{}\"", name);
    }
}

} // namespace miximus::nodes::decklink