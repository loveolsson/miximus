#include "registry.hpp"
#include "logger/logger.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <fmt/format.h>

namespace miximus::nodes::decklink {

#if _WIN32
static std::string wcs_tp_mbs(const wchar_t* pstr, long wslen)
{
    int len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, NULL, 0, NULL, NULL);

    std::string dblstr(len, '\0');
    len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, dblstr.data(), len, NULL, NULL);

    return dblstr;
}

static std::string bstr_to_mbs(BSTR bstr)
{
    int wslen = ::SysStringLen(bstr);
    return wcs_tp_mbs((wchar_t*)bstr, wslen);
}
#endif

static decklink_ptr<IDeckLinkDiscovery> get_device_discovery()
{
    IDeckLinkDiscovery* discovery = nullptr;

#if _WIN32

    auto res =
        CoCreateInstance(CLSID_CDeckLinkDiscovery, NULL, CLSCTX_ALL, IID_IDeckLinkDiscovery, (LPVOID*)&discovery);

    if (FAILED(res)) {
        discovery = nullptr;
    }
#else
    discovery     = CreateDeckLinkDiscoveryInstance();
#endif

    decklink_ptr ptr(discovery);

    if (discovery != nullptr) {
        discovery->Release();
    }

    return ptr;
}

static decklink_ptr<IDeckLinkVideoConversion> get_device_conversion()
{
    IDeckLinkVideoConversion* conversion = nullptr;

#if _WIN32

    auto res = CoCreateInstance(
        CLSID_CDeckLinkVideoConversion, NULL, CLSCTX_ALL, IID_IDeckLinkVideoConversion, (LPVOID*)&conversion);

    if (FAILED(res)) {
        conversion = nullptr;
    }
#else
    conversion    = CreateVideoConversionInstance();
#endif

    decklink_ptr ptr(conversion);

    if (conversion != nullptr) {
        conversion->Release();
    }

    return ptr;
}

static std::string get_decklink_name(decklink_ptr<IDeckLink>& device)
{
    std::string name;
    int64_t     id = 0;

#if _WIN32
    BSTR n;
    device->GetDisplayName(&n);
    ss << bstr_to_mbs(n);
    name = n;
#else
    const char* n = nullptr;
    device->GetDisplayName(&n);
    name             = n;
#endif

    /**
     * In some hardware configurations with multiple physical cards the devices will not have unique names,
     * and the order of the devices may change with reboots.
     * To combat this, the persistent ID of the device is appended to the name to serve as a unique and repeatable name.
     */
    decklink_ptr<IDeckLinkProfile>           profile(IID_IDeckLinkProfile, device);
    decklink_ptr<IDeckLinkProfileAttributes> attributes(IID_IDeckLinkProfileAttributes, profile);
    if (attributes) {
        if (FAILED(attributes->GetInt(BMDDeckLinkPersistentID, &id))) {
            id = -1;
        }
    }

    return fmt::format("{} [{}]", name, id);
}

std::string decklink_registry_s::get_display_mode_name(IDeckLinkDisplayMode* mode)
{
#if _WIN32
    BSTR name;
    mode->GetName(&name);
    return bstr_to_mbs(name);
#else
    const char* name = nullptr;
    mode->GetName(&name);
    return name;
#endif
}

class discovery_callback : public IDeckLinkDeviceNotificationCallback
{
    decklink_registry_s* registry_;

  public:
    explicit discovery_callback(decklink_registry_s* registry)
        : registry_(registry)
    {
    }

    HRESULT DeckLinkDeviceArrived(IDeckLink* deckLinkDevice) final
    {
        std::unique_lock lock(registry_->device_mutex_);

        auto log = getlog("decklink");

        decklink_ptr device(deckLinkDevice);

        auto                          name = get_decklink_name(device);
        decklink_ptr<IDeckLinkInput>  input(IID_IDeckLinkInput, device);
        decklink_ptr<IDeckLinkOutput> output(IID_IDeckLinkOutput, device);

        if (input) {
            registry_->inputs_.emplace(name, std::move(input));
            log->info("Discovered DeckLink input: \"{}\"", name);
        }

        if (output) {
            registry_->outputs_.emplace(name, std::move(output));
            log->info("Discovered DeckLink output: \"{}\"", name);
        }

        return S_OK;
    }

    HRESULT DeckLinkDeviceRemoved(IDeckLink* deckLinkDevice) final
    {
        std::unique_lock lock(registry_->device_mutex_);

        auto it = registry_->names_.find(deckLinkDevice);
        if (it == registry_->names_.end()) {
            return S_OK;
        }

        auto& name = it->second;
        registry_->inputs_.erase(name);
        registry_->outputs_.erase(name);
        registry_->names_.erase(it);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/) final { return E_NOTIMPL; }
    ULONG                     AddRef() final { return 1; }
    ULONG                     Release() final { return 1; }
};

decklink_registry_s::decklink_registry_s()
    : discovery_(get_device_discovery())
    , callback_(std::make_unique<discovery_callback>(this))
{
    std::unique_lock lock(device_mutex_);

    if (discovery_) {
        getlog("decklink")->debug("Installing DeckLink discovery");
        discovery_->InstallDeviceNotifications(callback_.get());
    }
}

decklink_registry_s::~decklink_registry_s()
{
    inputs_.clear();
    outputs_.clear();
}

void decklink_registry_s::uninstall()
{
    std::shared_lock lock(device_mutex_);

    if (discovery_) {
        getlog("decklink")->debug("Uninstalling DeckLink discovery");
        discovery_->UninstallDeviceNotifications();
    }
}

decklink_ptr<IDeckLinkInput> decklink_registry_s::get_input(const std::string& name)
{
    std::shared_lock lock(device_mutex_);
    auto             it = inputs_.find(name);
    if (it != inputs_.end()) {
        return it->second;
    }
    return {};
}

decklink_ptr<IDeckLinkOutput> decklink_registry_s::get_output(const std::string& name)
{
    std::shared_lock lock(device_mutex_);
    auto             it = outputs_.find(name);
    if (it != outputs_.end()) {
        return it->second;
    }

    return {};
}

std::vector<std::string> decklink_registry_s::get_input_names()
{
    std::shared_lock lock(device_mutex_);

    std::vector<std::string> res;
    res.reserve(inputs_.size());

    for (const auto& [name, _] : inputs_) {
        res.push_back(name);
    }

    return res;
}

std::vector<std::string> decklink_registry_s::get_output_names()
{
    std::shared_lock lock(device_mutex_);

    std::vector<std::string> res;
    res.reserve(outputs_.size());

    for (const auto& [name, _] : outputs_) {
        res.push_back(name);
    }

    return res;
}

decklink_ptr<IDeckLinkVideoConversion> decklink_registry_s::get_converter() { return get_device_conversion(); }

std::unique_ptr<decklink_registry_s> decklink_registry_s::create_decklink_registry()
{
    return std::make_unique<decklink_registry_s>();
}

} // namespace miximus::nodes::decklink