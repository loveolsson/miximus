#include "output_activation.hpp"

#include "logger/logger.hpp"
#include "utils/lookup.hpp"

#include <cstdint>
#include <utility>

namespace miximus::nodes::decklink::detail {

using namespace decklink_sdk;

namespace {
auto log() { return getlog("decklink"); }
} // namespace

output_activation_s::output_activation_s(decklink_ptr<IDeckLinkOutput> device, std::string device_name)
    : device_(std::move(device))
    , device_name_(std::move(device_name))
{
}

bool output_activation_s::supports_ordinary_output(BMDDisplayMode display_mode)
{
    bool supported{};
    if (device_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                      display_mode,
                                      bmdFormat10BitYUV,
                                      bmdNoVideoOutputConversion,
                                      bmdSupportedVideoModeDefault,
                                      nullptr,
                                      &supported) != S_OK ||
        !supported) {
        log()->error("DeckLink output mode is not supported by {}", device_name_);
        return false;
    }
    return true;
}

auto output_activation_s::keyed_output_fallback_reason(BMDDisplayMode display_mode, keyer_mode_e requested_keyer_mode)
    -> std::optional<std::string>
{
    auto       attributes = device_.query<IDeckLinkProfileAttributes>();
    bool       capability{};
    const auto capability_id = requested_keyer_mode == keyer_mode_e::external ? BMDDeckLinkSupportsExternalKeying
                                                                              : BMDDeckLinkSupportsInternalKeying;
    if (!attributes || attributes->GetFlag(capability_id, &capability) != S_OK || !capability) {
        return "the active device profile does not advertise that keyer capability";
    }

    keyer_ = device_.query<IDeckLinkKeyer>();
    if (!keyer_) {
        return "the device does not expose the DeckLink keyer interface";
    }

    bool supported{};
    if (device_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                      display_mode,
                                      bmdFormat8BitARGB,
                                      bmdNoVideoOutputConversion,
                                      bmdSupportedVideoModeKeying,
                                      nullptr,
                                      &supported) != S_OK ||
        !supported) {
        keyer_ = nullptr;
        return "the selected display mode does not support 8-bit ARGB keying";
    }
    return std::nullopt;
}

bool output_activation_s::enable_output(BMDDisplayMode display_mode)
{
    if (device_->EnableVideoOutput(display_mode, bmdVideoOutputFlagDefault) != S_OK) {
        log()->error("Failed to enable DeckLink output {}", device_name_);
        return false;
    }
    output_enabled_ = true;
    return true;
}

bool output_activation_s::enable_keyer(keyer_mode_e keyer_mode)
{
    const bool external      = keyer_mode == keyer_mode_e::external;
    const auto enable_result = keyer_->Enable(external);
    const auto level_result  = enable_result == S_OK ? keyer_->SetLevel(255) : E_FAIL;
    if (enable_result == S_OK && level_result == S_OK) {
        return true;
    }

    log()->error("DeckLink keyer setup failed for {}: Enable={:#010x}, SetLevel={:#010x}",
                 device_name_,
                 static_cast<uint32_t>(enable_result),
                 static_cast<uint32_t>(level_result));
    return false;
}

bool output_activation_s::restart_without_keyer(const output_display_mode_s& display_mode,
                                                active_output_s*             active_output)
{
    if (keyer_) {
        (void)keyer_->Disable();
    }
    if (output_enabled_) {
        if (device_->DisableVideoOutput() != S_OK) {
            log()->error("Unable to disable DeckLink output after keyer setup failed for {}", device_name_);
            return false;
        }
        output_enabled_ = false;
    }

    auto ordinary_path = v210_output_path_s::create(device_.get(), display_mode, device_name_);
    if (!ordinary_path || !enable_output(display_mode.mode)) {
        log()->error("Unable to restart ordinary DeckLink output after keyer setup failed for {}", device_name_);
        return false;
    }
    active_output->path              = std::move(ordinary_path);
    active_output->active_keyer_mode = keyer_mode_e::disabled;
    return true;
}

auto output_activation_s::start(const output_display_mode_s& display_mode, keyer_mode_e requested_keyer_mode)
    -> std::optional<active_output_s>
{
    if (!supports_ordinary_output(display_mode.mode)) {
        return std::nullopt;
    }

    active_output_s active_output{
        .path                  = {},
        .requested_keyer_mode  = requested_keyer_mode,
        .active_keyer_mode     = keyer_mode_e::disabled,
        .keyer_fallback_reason = std::nullopt,
    };

    if (requested_keyer_mode != keyer_mode_e::disabled) {
        active_output.keyer_fallback_reason = keyed_output_fallback_reason(display_mode.mode, requested_keyer_mode);
        if (!active_output.keyer_fallback_reason) {
            active_output.path = premultiplied_bgra_output_path_s::create(device_.get(), display_mode, device_name_);
            if (!active_output.path) {
                return std::nullopt;
            }
        }
    }
    if (!active_output.path) {
        active_output.path = v210_output_path_s::create(device_.get(), display_mode, device_name_);
        if (!active_output.path) {
            return std::nullopt;
        }
    }

    if (!keyer_) {
        keyer_ = device_.query<IDeckLinkKeyer>();
    }
    // DeckLink devices can retain keyer state across output restarts and
    // process lifetimes. Normalize it before changing the output mode.
    if (keyer_) {
        (void)keyer_->Disable();
    }
    if (!enable_output(display_mode.mode)) {
        return std::nullopt;
    }

    if (!active_output.keyer_fallback_reason && requested_keyer_mode != keyer_mode_e::disabled) {
        if (enable_keyer(requested_keyer_mode)) {
            active_output.active_keyer_mode = requested_keyer_mode;
        } else {
            active_output.keyer_fallback_reason = "the DeckLink driver rejected enabling the keyer";
            if (!restart_without_keyer(display_mode, &active_output)) {
                return std::nullopt;
            }
        }
    } else if (keyer_ && keyer_->Disable() != S_OK) {
        log()->warn("Unable to confirm that keying is disabled on {}; continuing with ordinary 10-bit YUV output",
                    device_name_);
    }

    if (active_output.keyer_fallback_reason) {
        log()->warn("DeckLink output {} requested {} keying but will continue with ordinary 10-bit YUV output: {}",
                    device_name_,
                    enum_to_string(requested_keyer_mode),
                    *active_output.keyer_fallback_reason);
    }
    return active_output;
}

void output_activation_s::stop()
{
    if (keyer_) {
        (void)keyer_->Disable();
    }
    if (device_ && output_enabled_) {
        (void)device_->DisableVideoOutput();
        output_enabled_ = false;
    }
    keyer_ = nullptr;
}

} // namespace miximus::nodes::decklink::detail
