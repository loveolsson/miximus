#include "registry.hpp"
#include "logger/logger.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <sstream>

namespace miximus::nodes::decklink {

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

static decklink_ptr<IDeckLinkDiscovery> get_device_discovery()
{
#if _WIN32
    IDeckLinkDiscovery* discovery = nullptr;

    auto res =
        CoCreateInstance(CLSID_CDeckLinkDiscovery, NULL, CLSCTX_ALL, IID_IDeckLinkDiscovery, (LPVOID*)&discovery);

    if (SUCCEEDED(res)) {
        return discovery;
    }

    return nullptr;
#else
    return CreateDeckLinkDiscoveryInstance();
#endif
}

static std::string get_decklink_name(const decklink_ptr<IDeckLink>& device)
{
    std::stringstream ss;

#if _WIN32
    BSTR name;
    device->GetDisplayName(&name);
    ss << bstr_to_mbs(name);
#else
    const char* name = nullptr;
    device->GetDisplayName(&name);
    ss << name;
#endif

    /**
     * In some hardware configurations with multiple physical cards the devices will not have unique names,
     * and the order of the devices may change with reboots.
     * To combat this, the persistent ID of the device is appended to the name to serve as a unique and repeatable name.
     */
    auto profile    = QUERY_INTERFACE(device, IDeckLinkProfile);
    auto attributes = QUERY_INTERFACE(profile, IDeckLinkProfileAttributes);
    if (attributes) {
        int64_t id = 0;
        if (SUCCEEDED(attributes->GetInt(BMDDeckLinkPersistentID, &id))) {
            ss << " [" << id << "]";
        }
    }

    return ss.str();
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

        auto device = decklink_ptr<IDeckLink>::make_owner(deckLinkDevice);

        auto name   = get_decklink_name(device);
        auto input  = QUERY_INTERFACE(device, IDeckLinkInput);
        auto output = QUERY_INTERFACE(device, IDeckLinkOutput);

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
    return nullptr;
}

decklink_ptr<IDeckLinkOutput> decklink_registry_s::get_output(const std::string& name)
{
    std::shared_lock lock(device_mutex_);
    auto             it = outputs_.find(name);
    if (it != outputs_.end()) {
        return it->second;
    }
    return nullptr;
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

std::unique_ptr<decklink_registry_s> decklink_registry_s::create_decklink_registry()
{
    return std::make_unique<decklink_registry_s>();
}

} // namespace miximus::nodes::decklink