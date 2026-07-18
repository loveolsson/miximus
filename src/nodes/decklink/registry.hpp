#pragma once
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

struct IDeckLink;
struct IDeckLinkInput;
struct IDeckLinkOutput;
struct IDeckLinkDiscovery;
struct IDeckLinkDeviceNotificationCallback;
struct IDeckLinkVideoConversion;
struct IDeckLinkDisplayMode;

namespace miximus::nodes::decklink {

class discovery_callback;

class decklink_registry_s
{
    decklink_ptr<IDeckLinkDiscovery>                  discovery_;
    decklink_ptr<IDeckLinkDeviceNotificationCallback> callback_;
    bool                                              notifications_installed_{};

    std::shared_mutex                                                 device_mutex_;
    std::map<IDeckLink*, std::string>                                 names_;
    std::map<std::string, decklink_ptr<IDeckLinkInput>, std::less<>>  inputs_;
    std::map<std::string, decklink_ptr<IDeckLinkOutput>, std::less<>> outputs_;
    std::atomic<uint64_t>                                             device_list_version_{0};

    friend class discovery_callback;

  public:
    decklink_registry_s();
    ~decklink_registry_s();

    void uninstall();

    decklink_ptr<IDeckLinkInput>  get_input(std::string_view name);
    decklink_ptr<IDeckLinkOutput> get_output(std::string_view name);

    uint64_t get_device_list_version() { return device_list_version_.load(std::memory_order_relaxed); }

    std::vector<std::string> get_input_names();
    std::vector<std::string> get_output_names();

    static std::string                            get_display_mode_name(IDeckLinkDisplayMode* mode);
    static decklink_ptr<IDeckLinkVideoConversion> get_converter();

    static std::unique_ptr<decklink_registry_s> create_decklink_registry();
};

} // namespace miximus::nodes::decklink
