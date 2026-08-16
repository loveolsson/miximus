#pragma once

#include "output_path.hpp"
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <optional>
#include <string>

namespace miximus::nodes::decklink::detail {

class output_activation_s
{
    decklink_sdk::decklink_ptr<IDeckLinkOutput> device_;
    decklink_sdk::decklink_ptr<IDeckLinkKeyer>  keyer_;
    std::string                                 device_name_;
    bool                                        output_enabled_{};

    bool supports_ordinary_output(BMDDisplayMode display_mode);
    auto keyed_output_fallback_reason(BMDDisplayMode display_mode, keyer_mode_e requested_keyer_mode)
        -> std::optional<std::string>;
    bool enable_output(BMDDisplayMode display_mode);
    bool enable_keyer(keyer_mode_e keyer_mode);
    bool restart_without_keyer(const output_display_mode_s& display_mode, active_output_s* active_output);

  public:
    output_activation_s(decklink_sdk::decklink_ptr<IDeckLinkOutput> device, std::string device_name);
    ~output_activation_s() = default;

    output_activation_s(const output_activation_s&)            = delete;
    output_activation_s& operator=(const output_activation_s&) = delete;
    output_activation_s(output_activation_s&&)                 = delete;
    output_activation_s& operator=(output_activation_s&&)      = delete;

    auto start(const output_display_mode_s& display_mode, keyer_mode_e requested_keyer_mode)
        -> std::optional<active_output_s>;
    void stop();
};

} // namespace miximus::nodes::decklink::detail
