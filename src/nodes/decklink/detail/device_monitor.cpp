#include "device_monitor.hpp"

#include "logger/logger.hpp"
#include "platform_compat.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace miximus::nodes::decklink::detail {
namespace {

std::string fourcc(uint32_t value)
{
    std::string result(4, ' ');
    result[0] = static_cast<char>((value >> 24U) & 0xffU);
    result[1] = static_cast<char>((value >> 16U) & 0xffU);
    result[2] = static_cast<char>((value >> 8U) & 0xffU);
    result[3] = static_cast<char>(value & 0xffU);
    return result;
}

std::string colorspace_name(int64_t value)
{
    switch (static_cast<BMDColorspace>(value)) {
        case bmdColorspaceRec601:
            return "Rec. 601";
        case bmdColorspaceRec709:
            return "Rec. 709";
        case bmdColorspaceRec2020:
            return "Rec. 2020";
        case bmdColorspaceDolbyVisionNative:
            return "Dolby Vision";
        case bmdColorspaceP3D65:
            return "P3-D65";
        case bmdColorspaceUnknown:
            return "Unknown";
        default:
            return fourcc(static_cast<uint32_t>(value));
    }
}

std::string dynamic_range_name(int64_t value)
{
    if (value == bmdDynamicRangeSDR) {
        return "SDR";
    }

    std::string result;
    if ((value & bmdDynamicRangeHDRStaticPQ) != 0) {
        result = "HDR PQ";
    }
    if ((value & bmdDynamicRangeHDRStaticHLG) != 0) {
        if (!result.empty()) {
            result += ", ";
        }
        result += "HDR HLG";
    }
    return result.empty() ? fourcc(static_cast<uint32_t>(value)) : result;
}

std::string link_configuration_name(int64_t value)
{
    switch (static_cast<BMDLinkConfiguration>(value)) {
        case bmdLinkConfigurationSingleLink:
            return "Single link";
        case bmdLinkConfigurationDualLink:
            return "Dual link";
        case bmdLinkConfigurationQuadLink:
            return "Quad link";
        default:
            return fourcc(static_cast<uint32_t>(value));
    }
}

std::string field_dominance_name(int64_t value)
{
    switch (static_cast<BMDFieldDominance>(value)) {
        case bmdUnknownFieldDominance:
            return "Unknown";
        case bmdLowerFieldFirst:
            return "Lower field first";
        case bmdUpperFieldFirst:
            return "Upper field first";
        case bmdProgressiveFrame:
            return "Progressive";
        case bmdProgressiveSegmentedFrame:
            return "Progressive segmented frame";
        default:
            return fourcc(static_cast<uint32_t>(value));
    }
}

template <typename T>
bool assign_if_changed(std::optional<T>* destination, std::optional<T> value)
{
    if (*destination == value) {
        return false;
    }
    *destination = std::move(value);
    return true;
}

struct monitor_state_s
{
    decklink_ptr<IDeckLinkStatus>     status;
    decklink_ptr<IDeckLinkStatistics> statistics;
    decklink_ptr<IDeckLinkInput>      input;
    decklink_ptr<IDeckLinkOutput>     output;

    mutable std::mutex                     mutex;
    std::shared_ptr<const device_status_s> snapshot = std::make_shared<device_status_s>();
};

std::optional<bool> read_flag(IDeckLinkStatus* status, BMDDeckLinkStatusID id)
{
    bool value{};
    return status != nullptr && status->GetFlag(id, &value) == S_OK ? std::optional(value) : std::nullopt;
}

std::optional<int64_t> read_int(IDeckLinkStatus* status, BMDDeckLinkStatusID id)
{
    int64_t value{};
    return status != nullptr && status->GetInt(id, &value) == S_OK ? std::optional(value) : std::nullopt;
}

std::optional<std::string>
read_mode(IDeckLinkStatus* status, IDeckLinkInput* input, IDeckLinkOutput* output, BMDDeckLinkStatusID id)
{
    const auto value = read_int(status, id);
    if (!value.has_value()) {
        return std::nullopt;
    }

    decklink_ptr<IDeckLinkDisplayMode> mode;
    const auto                         display_mode = static_cast<BMDDisplayMode>(*value);
    HRESULT                            result       = E_FAIL;
    if (id == bmdDeckLinkStatusDetectedVideoInputMode || id == bmdDeckLinkStatusCurrentVideoInputMode) {
        result = input != nullptr ? input->GetDisplayMode(display_mode, mode.releaseAndGetAddressOf()) : E_FAIL;
    } else {
        result = output != nullptr ? output->GetDisplayMode(display_mode, mode.releaseAndGetAddressOf()) : E_FAIL;
    }
    return result == S_OK && mode ? std::optional(get_display_mode_name(mode.get()))
                                  : std::optional(fourcc(static_cast<uint32_t>(*value)));
}

void refresh_status(const std::shared_ptr<monitor_state_s>& state, BMDDeckLinkStatusID id)
{
    if (!state->status) {
        return;
    }

    std::scoped_lock lock(state->mutex);
    auto             snapshot = *state->snapshot;
    bool             changed  = false;

    switch (id) {
        case bmdDeckLinkStatusVideoInputSignalLocked:
            changed = assign_if_changed(&snapshot.input_signal_locked, read_flag(state->status.get(), id));
            break;
        case bmdDeckLinkStatusAncillaryInputSignalLocked:
            changed = assign_if_changed(&snapshot.ancillary_signal_locked, read_flag(state->status.get(), id));
            break;
        case bmdDeckLinkStatusReferenceSignalLocked:
            changed = assign_if_changed(&snapshot.reference_signal_locked, read_flag(state->status.get(), id));
            break;
        case bmdDeckLinkStatusBusy: {
            const auto value = read_int(state->status.get(), id);
            changed |= assign_if_changed(&snapshot.capture_busy,
                                         value.has_value() ? std::optional((*value & bmdDeviceCaptureBusy) != 0)
                                                           : std::nullopt);
            changed |= assign_if_changed(&snapshot.playback_busy,
                                         value.has_value() ? std::optional((*value & bmdDevicePlaybackBusy) != 0)
                                                           : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusPCIExpressLinkWidth:
            changed = assign_if_changed(&snapshot.pcie_link_width, read_int(state->status.get(), id));
            break;
        case bmdDeckLinkStatusPCIExpressLinkSpeed:
            changed = assign_if_changed(&snapshot.pcie_link_speed, read_int(state->status.get(), id));
            break;
        case bmdDeckLinkStatusDetectedVideoInputMode:
            changed = assign_if_changed(&snapshot.detected_input_mode,
                                        read_mode(state->status.get(), state->input.get(), state->output.get(), id));
            break;
        case bmdDeckLinkStatusCurrentVideoInputMode:
            changed = assign_if_changed(&snapshot.current_input_mode,
                                        read_mode(state->status.get(), state->input.get(), state->output.get(), id));
            break;
        case bmdDeckLinkStatusCurrentVideoOutputMode:
            changed = assign_if_changed(&snapshot.current_output_mode,
                                        read_mode(state->status.get(), state->input.get(), state->output.get(), id));
            break;
        case bmdDeckLinkStatusReferenceSignalMode:
            changed = assign_if_changed(&snapshot.reference_signal_mode,
                                        read_mode(state->status.get(), state->input.get(), state->output.get(), id));
            break;
        case bmdDeckLinkStatusDetectedVideoInputColorspace: {
            const auto value = read_int(state->status.get(), id);
            changed          = assign_if_changed(&snapshot.detected_colorspace,
                                        value.has_value() ? std::optional(colorspace_name(*value)) : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusDetectedVideoInputDynamicRange: {
            const auto value = read_int(state->status.get(), id);
            changed          = assign_if_changed(&snapshot.detected_dynamic_range,
                                        value.has_value() ? std::optional(dynamic_range_name(*value)) : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusDetectedVideoInputFieldDominance: {
            const auto value = read_int(state->status.get(), id);
            changed          = assign_if_changed(&snapshot.detected_field_dominance,
                                        value.has_value() ? std::optional(field_dominance_name(*value)) : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusDetectedSDILinkConfiguration: {
            const auto value = read_int(state->status.get(), id);
            changed =
                assign_if_changed(&snapshot.detected_sdi_link_configuration,
                                  value.has_value() ? std::optional(link_configuration_name(*value)) : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusCurrentVideoInputPixelFormat: {
            const auto value = read_int(state->status.get(), id);
            changed          = assign_if_changed(&snapshot.current_input_pixel_format,
                                        value.has_value() ? std::optional(fourcc(static_cast<uint32_t>(*value)))
                                                                   : std::nullopt);
            break;
        }
        case bmdDeckLinkStatusLastVideoOutputPixelFormat: {
            const auto value = read_int(state->status.get(), id);
            changed          = assign_if_changed(&snapshot.last_output_pixel_format,
                                        value.has_value() ? std::optional(fourcc(static_cast<uint32_t>(*value)))
                                                                   : std::nullopt);
            break;
        }
        default:
            break;
    }

    if (changed) {
        ++snapshot.version;
        state->snapshot = std::make_shared<device_status_s>(std::move(snapshot));
    }
}

class notification_callback_s final : public IDeckLinkNotificationCallback
{
    std::atomic_ulong                ref_count_{1};
    std::shared_ptr<monitor_state_s> state_;

  public:
    explicit notification_callback_s(std::shared_ptr<monitor_state_s> state)
        : state_(std::move(state))
    {
    }

    HRESULT STDMETHODCALLTYPE Notify(BMDNotifications topic, uint64_t param1, uint64_t /*param2*/) final
    {
        if (topic != bmdStatusChanged) {
            return S_OK;
        }
        try {
            refresh_status(state_, static_cast<BMDDeckLinkStatusID>(param1));
        } catch (...) {
            getlog("decklink")->error("DeckLink status notification callback failed");
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkNotificationCallback>(iid)) {
            *ppv = static_cast<IDeckLinkNotificationCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() final { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() final
    {
        const auto count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }
};

constexpr std::array monitored_statuses{
    bmdDeckLinkStatusVideoInputSignalLocked,
    bmdDeckLinkStatusAncillaryInputSignalLocked,
    bmdDeckLinkStatusReferenceSignalLocked,
    bmdDeckLinkStatusBusy,
    bmdDeckLinkStatusPCIExpressLinkWidth,
    bmdDeckLinkStatusPCIExpressLinkSpeed,
    bmdDeckLinkStatusDetectedVideoInputMode,
    bmdDeckLinkStatusDetectedVideoInputColorspace,
    bmdDeckLinkStatusDetectedVideoInputDynamicRange,
    bmdDeckLinkStatusDetectedVideoInputFieldDominance,
    bmdDeckLinkStatusDetectedSDILinkConfiguration,
    bmdDeckLinkStatusCurrentVideoInputMode,
    bmdDeckLinkStatusCurrentVideoInputPixelFormat,
    bmdDeckLinkStatusCurrentVideoOutputMode,
    bmdDeckLinkStatusLastVideoOutputPixelFormat,
    bmdDeckLinkStatusReferenceSignalMode,
};

} // namespace

class device_monitor_s::impl_s
{
  public:
    std::shared_ptr<monitor_state_s>      state = std::make_shared<monitor_state_s>();
    decklink_ptr<IDeckLinkNotification>   notification;
    decklink_ptr<notification_callback_s> callback;
    bool                                  subscribed{};
};

device_monitor_s::device_monitor_s(IDeckLink* device)
    : impl_(std::make_unique<impl_s>())
{
    decklink_ptr device_ptr(device);
    impl_->state->status     = device_ptr.query<IDeckLinkStatus>();
    impl_->state->statistics = device_ptr.query<IDeckLinkStatistics>();
    impl_->state->input      = device_ptr.query<IDeckLinkInput>();
    impl_->state->output     = device_ptr.query<IDeckLinkOutput>();

    for (const auto id : monitored_statuses) {
        refresh_status(impl_->state, id);
    }
    poll_statistics();

    impl_->notification = device_ptr.query<IDeckLinkNotification>();
    if (impl_->notification && impl_->state->status) {
        impl_->callback   = make_decklink_ptr<notification_callback_s>(impl_->state);
        impl_->subscribed = impl_->notification->Subscribe(bmdStatusChanged, impl_->callback.get()) == S_OK;
        if (!impl_->subscribed) {
            getlog("decklink")->warn("Failed to subscribe to DeckLink device status changes");
        }
    }
}

device_monitor_s::~device_monitor_s()
{
    if (impl_->subscribed) {
        (void)impl_->notification->Unsubscribe(bmdStatusChanged, impl_->callback.get());
    }
}

void device_monitor_s::poll_statistics()
{
    if (!impl_->state->statistics) {
        return;
    }

    int64_t                value{};
    std::optional<int64_t> temperature;
    if (impl_->state->statistics->GetInt(bmdDeckLinkStatisticDeviceTemperature, &value) == S_OK) {
        temperature = value;
    }

    const std::scoped_lock lock(impl_->state->mutex);
    auto                   snapshot = *impl_->state->snapshot;
    if (assign_if_changed(&snapshot.temperature_c, temperature)) {
        ++snapshot.version;
        impl_->state->snapshot = std::make_shared<device_status_s>(std::move(snapshot));
    }
}

std::shared_ptr<const device_status_s> device_monitor_s::status() const
{
    const std::scoped_lock lock(impl_->state->mutex);
    return impl_->state->snapshot;
}

} // namespace miximus::nodes::decklink::detail
