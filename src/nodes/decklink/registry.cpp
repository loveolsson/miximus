#include "registry.hpp"

#include "detail/device_monitor.hpp"
#include "logger/logger.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace miximus::nodes::decklink {
using namespace miximus::decklink_sdk;

using namespace std::chrono_literals;

namespace {
std::string get_decklink_name(decklink_ptr<IDeckLink>& device)
{
    auto    name = get_device_display_name(device.get());
    int64_t id   = 0;

    /**
     * In some hardware configurations with multiple physical cards the devices will not have unique names,
     * and the order of the devices may change with reboots.
     * To combat this, the persistent ID of the device is appended to the name to serve as a unique and repeatable name.
     */
    const auto profile    = device.query<IDeckLinkProfile>();
    auto       attributes = profile.query<IDeckLinkProfileAttributes>();
    if (attributes) {
        if (FAILED(attributes->GetInt(BMDDeckLinkPersistentID, &id))) {
            id = -1;
        }
    }

    return std::format("{} [{}]", name, id);
}
} // namespace

class discovery_callback : public IDeckLinkDeviceNotificationCallback
{
    decklink_registry_s* registry_;
    std::atomic_ulong    ref_count_{1};

  public:
    explicit discovery_callback(decklink_registry_s* registry)
        : registry_(registry)
    {
    }

    HRESULT DeckLinkDeviceArrived(IDeckLink* deckLinkDevice) final
    {
        if (deckLinkDevice == nullptr) {
            return E_INVALIDARG;
        }

        try {
            return device_arrived(deckLinkDevice);
        } catch (const std::exception& error) {
            getlog("decklink")->error("DeckLink device arrival callback failed: {}", error.what());
        } catch (...) {
            getlog("decklink")->error("DeckLink device arrival callback failed");
        }
        return E_FAIL;
    }

    HRESULT DeckLinkDeviceRemoved(IDeckLink* deckLinkDevice) final
    {
        if (deckLinkDevice == nullptr) {
            return E_INVALIDARG;
        }

        try {
            return device_removed(deckLinkDevice);
        } catch (const std::exception& error) {
            getlog("decklink")->error("DeckLink device removal callback failed: {}", error.what());
        } catch (...) {
            getlog("decklink")->error("DeckLink device removal callback failed");
        }
        return E_FAIL;
    }

  private:
    HRESULT device_arrived(IDeckLink* deckLinkDevice)
    {
        decklink_ptr device(deckLinkDevice);

        auto       name       = get_decklink_name(device);
        auto       input      = device.query<IDeckLinkInput>();
        auto       output     = device.query<IDeckLinkOutput>();
        auto       monitor    = std::make_shared<detail::device_monitor_s>(device.get());
        const bool has_input  = input != nullptr;
        const bool has_output = output != nullptr;

        {
            const std::unique_lock lock(registry_->device_mutex_);
            registry_->names_.insert_or_assign(deckLinkDevice, name);
            registry_->monitors_.insert_or_assign(name, std::move(monitor));

            if (input) {
                registry_->inputs_.insert_or_assign(name, std::move(input));
            }

            if (output) {
                registry_->outputs_.insert_or_assign(name, std::move(output));
            }
        }

        auto log = getlog("decklink");

        if (has_input) {
            log->info("Discovered DeckLink input: \"{}\"", name);
        }
        if (has_output) {
            log->info("Discovered DeckLink output: \"{}\"", name);
        }

        ++registry_->device_list_version_;
        return S_OK;
    }

    HRESULT device_removed(IDeckLink* deckLinkDevice)
    {
        std::string                               name;
        decklink_ptr<IDeckLinkInput>              removed_input;
        decklink_ptr<IDeckLinkOutput>             removed_output;
        std::shared_ptr<detail::device_monitor_s> removed_monitor;
        {
            const std::unique_lock lock(registry_->device_mutex_);

            auto it = registry_->names_.find(deckLinkDevice);
            if (it == registry_->names_.end()) {
                return S_OK;
            }

            name = std::move(it->second);
            if (auto input = registry_->inputs_.find(name); input != registry_->inputs_.end()) {
                removed_input = std::move(input->second);
                registry_->inputs_.erase(input);
            }
            if (auto output = registry_->outputs_.find(name); output != registry_->outputs_.end()) {
                removed_output = std::move(output->second);
                registry_->outputs_.erase(output);
            }
            if (auto monitor = registry_->monitors_.find(name); monitor != registry_->monitors_.end()) {
                removed_monitor = std::move(monitor->second);
                registry_->monitors_.erase(monitor);
            }
            registry_->names_.erase(it);
        }

        ++registry_->device_list_version_;
        getlog("decklink")->info("DeckLink device removed: \"{}\"", name);
        return S_OK;
    }

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkDeviceNotificationCallback>(iid)) {
            *ppv = static_cast<IDeckLinkDeviceNotificationCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG AddRef() final { return ++ref_count_; }
    ULONG Release() final
    {
        const auto count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }
};

decklink_registry_s::decklink_registry_s()
    : discovery_(create_device_discovery())
    , callback_(new discovery_callback(this), false)
    , control_executor_(std::make_unique<utils::serial_executor_s>())
{
    if (discovery_) {
        getlog("decklink")->debug("Installing DeckLink discovery");
        notifications_installed_ = discovery_->InstallDeviceNotifications(callback_.get()) == S_OK;
        if (!notifications_installed_) {
            getlog("decklink")->error("Failed to install DeckLink device notifications");
        }
    }

    statistics_thread_ = std::jthread([this](const std::stop_token& stop_token) {
        std::mutex                  wait_mutex;
        std::condition_variable_any wait_condition;
        std::unique_lock            wait_lock(wait_mutex);

        while (!stop_token.stop_requested()) {
            wait_condition.wait_for(wait_lock, stop_token, 2s, [] { return false; });
            if (stop_token.stop_requested()) {
                break;
            }

            std::vector<std::shared_ptr<detail::device_monitor_s>> monitors;
            {
                const std::shared_lock lock(device_mutex_);
                monitors.reserve(monitors_.size());
                for (const auto& [_, monitor] : monitors_) {
                    monitors.push_back(monitor);
                }
            }
            for (const auto& monitor : monitors) {
                monitor->poll_statistics();
            }
        }
    });
}

decklink_registry_s::~decklink_registry_s()
{
    uninstall();
    control_executor_.reset();
    statistics_thread_.request_stop();
    statistics_thread_.join();
    const std::unique_lock lock(device_mutex_);
    names_.clear();
    inputs_.clear();
    outputs_.clear();
    monitors_.clear();
}

void decklink_registry_s::uninstall()
{
    if (!discovery_ || !notifications_installed_) {
        return;
    }

    getlog("decklink")->debug("Uninstalling DeckLink discovery");
    const auto result = discovery_->UninstallDeviceNotifications();
    if (result != S_OK) {
        getlog("decklink")->warn("Failed to uninstall DeckLink device notifications: {:#x}", result);
    }
    notifications_installed_ = false;
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

std::shared_ptr<const device_status_s> decklink_registry_s::get_device_status(std::string_view name)
{
    std::shared_ptr<detail::device_monitor_s> monitor;
    {
        const std::shared_lock lock(device_mutex_);
        const auto             it = monitors_.find(name);
        if (it == monitors_.end()) {
            return {};
        }
        monitor = it->second;
    }
    return monitor->status();
}

std::vector<settings_option_s> decklink_registry_s::get_input_options()
{
    const std::shared_lock lock(device_mutex_);

    std::vector<settings_option_s> options;
    options.reserve(inputs_.size());

    for (const auto& [name, _] : inputs_) {
        options.push_back({.id = name, .label = name});
    }

    return options;
}

std::vector<settings_option_s> decklink_registry_s::get_output_options()
{
    const std::shared_lock lock(device_mutex_);

    std::vector<settings_option_s> options;
    options.reserve(outputs_.size());

    for (const auto& [name, _] : outputs_) {
        options.push_back({.id = name, .label = name});
    }

    return options;
}

std::unique_ptr<decklink_registry_s> decklink_registry_s::create_decklink_registry()
{
    return std::make_unique<decklink_registry_s>();
}

} // namespace miximus::nodes::decklink
