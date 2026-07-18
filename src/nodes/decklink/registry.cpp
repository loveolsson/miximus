#include "registry.hpp"

#include "detail/platform_compat.hpp"
#include "logger/logger.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace miximus::nodes::decklink {

namespace {
std::string get_decklink_name(decklink_ptr<IDeckLink>& device)
{
    auto    name = detail::get_device_display_name(device.get());
    int64_t id   = 0;

    /**
     * In some hardware configurations with multiple physical cards the devices will not have unique names,
     * and the order of the devices may change with reboots.
     * To combat this, the persistent ID of the device is appended to the name to serve as a unique and repeatable name.
     */
    const decklink_ptr<IDeckLinkProfile>     profile(IID_IDeckLinkProfile, device);
    decklink_ptr<IDeckLinkProfileAttributes> attributes(IID_IDeckLinkProfileAttributes, profile);
    if (attributes) {
        if (FAILED(attributes->GetInt(BMDDeckLinkPersistentID, &id))) {
            id = -1;
        }
    }

    return std::format("{} [{}]", name, id);
}
} // namespace

std::string decklink_registry_s::get_display_mode_name(IDeckLinkDisplayMode* mode)
{
    return detail::get_display_mode_name(mode);
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
        auto log = getlog("decklink");

        decklink_ptr device(deckLinkDevice);

        auto                          name = get_decklink_name(device);
        decklink_ptr<IDeckLinkInput>  input(IID_IDeckLinkInput, device);
        decklink_ptr<IDeckLinkOutput> output(IID_IDeckLinkOutput, device);
        const bool                    has_input  = input != nullptr;
        const bool                    has_output = output != nullptr;

        {
            const std::unique_lock lock(registry_->device_mutex_);
            registry_->names_.insert_or_assign(deckLinkDevice, name);

            if (input) {
                registry_->inputs_.insert_or_assign(name, std::move(input));
            }

            if (output) {
                registry_->outputs_.insert_or_assign(name, std::move(output));
            }
        }

        if (has_input) {
            log->info("Discovered DeckLink input: \"{}\"", name);
        }
        if (has_output) {
            log->info("Discovered DeckLink output: \"{}\"", name);
        }

        ++registry_->device_list_version_;
        return S_OK;
    }

    HRESULT DeckLinkDeviceRemoved(IDeckLink* deckLinkDevice) final
    {
        const std::unique_lock lock(registry_->device_mutex_);

        auto it = registry_->names_.find(deckLinkDevice);
        if (it == registry_->names_.end()) {
            return S_OK;
        }

        auto& name = it->second;
        registry_->inputs_.erase(name);
        registry_->outputs_.erase(name);
        registry_->names_.erase(it);
        ++registry_->device_list_version_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/) final { return E_NOTIMPL; }
    ULONG                     AddRef() final { return 1; }
    ULONG                     Release() final { return 1; }
};

decklink_registry_s::decklink_registry_s()
    : discovery_(detail::create_device_discovery())
    , callback_(std::make_unique<discovery_callback>(this))
{
    const std::unique_lock lock(device_mutex_);

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
    const std::shared_lock lock(device_mutex_);

    if (!discovery_) {
        return;
    }

    getlog("decklink")->debug("Uninstalling DeckLink discovery");

    std::promise<void> done;
    auto               future = done.get_future();

    std::thread([this, p = std::move(done)]() mutable {
        discovery_->UninstallDeviceNotifications();
        p.set_value();
    }).detach();

    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        getlog("decklink")->warn("DeckLink uninstall timed out — kernel module may need rebuilding (dkms autoinstall)");
    }
}

decklink_ptr<IDeckLinkInput> decklink_registry_s::get_input(std::string_view name)
{
    const std::shared_lock lock(device_mutex_);
    auto                   it = inputs_.find(name);
    if (it != inputs_.end()) {
        return it->second;
    }
    return {};
}

decklink_ptr<IDeckLinkOutput> decklink_registry_s::get_output(std::string_view name)
{
    const std::shared_lock lock(device_mutex_);
    auto                   it = outputs_.find(name);
    if (it != outputs_.end()) {
        return it->second;
    }

    return {};
}

std::vector<std::string> decklink_registry_s::get_input_names()
{
    const std::shared_lock lock(device_mutex_);

    std::vector<std::string> res;
    res.reserve(inputs_.size());

    for (const auto& [name, _] : inputs_) {
        res.push_back(name);
    }

    return res;
}

std::vector<std::string> decklink_registry_s::get_output_names()
{
    const std::shared_lock lock(device_mutex_);

    std::vector<std::string> res;
    res.reserve(outputs_.size());

    for (const auto& [name, _] : outputs_) {
        res.push_back(name);
    }

    return res;
}

decklink_ptr<IDeckLinkVideoConversion> decklink_registry_s::get_converter()
{
    return detail::create_video_conversion();
}

std::unique_ptr<decklink_registry_s> decklink_registry_s::create_decklink_registry()
{
    return std::make_unique<decklink_registry_s>();
}

} // namespace miximus::nodes::decklink
