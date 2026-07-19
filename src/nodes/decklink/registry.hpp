#pragma once
#include "types/settings_option.hpp"
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
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
namespace detail {
class device_monitor_s;
}

struct device_status_s
{
    uint64_t version{};

    std::optional<bool>    input_signal_locked;
    std::optional<bool>    ancillary_signal_locked;
    std::optional<bool>    reference_signal_locked;
    std::optional<bool>    capture_busy;
    std::optional<bool>    playback_busy;
    std::optional<int64_t> pcie_link_width;
    std::optional<int64_t> pcie_link_speed;
    std::optional<int64_t> temperature_c;

    std::optional<std::string> detected_input_mode;
    std::optional<std::string> detected_colorspace;
    std::optional<std::string> detected_dynamic_range;
    std::optional<std::string> detected_field_dominance;
    std::optional<std::string> detected_sdi_link_configuration;
    std::optional<std::string> current_input_mode;
    std::optional<std::string> current_input_pixel_format;
    std::optional<std::string> current_output_mode;
    std::optional<std::string> last_output_pixel_format;
    std::optional<std::string> reference_signal_mode;
};

class decklink_registry_s
{
    decklink_ptr<IDeckLinkDiscovery>                  discovery_;
    decklink_ptr<IDeckLinkDeviceNotificationCallback> callback_;
    bool                                              notifications_installed_{};

    std::shared_mutex                                                             device_mutex_;
    std::map<IDeckLink*, std::string>                                             names_;
    std::map<std::string, decklink_ptr<IDeckLinkInput>, std::less<>>              inputs_;
    std::map<std::string, decklink_ptr<IDeckLinkOutput>, std::less<>>             outputs_;
    std::map<std::string, std::shared_ptr<detail::device_monitor_s>, std::less<>> monitors_;
    std::atomic<uint64_t>                                                         device_list_version_{0};
    std::jthread                                                                  statistics_thread_;

    friend class discovery_callback;

  public:
    decklink_registry_s();
    ~decklink_registry_s();

    void uninstall();

    decklink_ptr<IDeckLinkInput>           get_input(std::string_view name);
    decklink_ptr<IDeckLinkOutput>          get_output(std::string_view name);
    std::shared_ptr<const device_status_s> get_device_status(std::string_view name);

    uint64_t get_device_list_version() { return device_list_version_.load(std::memory_order_relaxed); }

    std::vector<settings_option_s> get_input_options();
    std::vector<settings_option_s> get_output_options();

    static std::string                            get_display_mode_name(IDeckLinkDisplayMode* mode);
    static decklink_ptr<IDeckLinkVideoConversion> get_converter();

    static std::unique_ptr<decklink_registry_s> create_decklink_registry();
};

} // namespace miximus::nodes::decklink
