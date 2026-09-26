#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
#include "detail/device_reservation.hpp"
#include "detail/input_capture.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/drawing.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::decklink_sdk;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace miximus::nodes::decklink::detail;

auto log() { return getlog("decklink"); }

status::decklink_input_device_status_s make_device_status(const device_status_s& status)
{
    return {
        .signal_locked                   = status.input_signal_locked,
        .ancillary_signal_locked         = status.ancillary_signal_locked,
        .capture_busy                    = status.capture_busy,
        .pcie_link_width                 = status.pcie_link_width,
        .pcie_link_speed                 = status.pcie_link_speed,
        .temperature_c                   = status.temperature_c,
        .active_format                   = status.current_input_mode,
        .detected_format                 = status.detected_input_mode,
        .detected_colorspace             = status.detected_colorspace,
        .detected_dynamic_range          = status.detected_dynamic_range,
        .detected_field_dominance        = status.detected_field_dominance,
        .detected_sdi_link_configuration = status.detected_sdi_link_configuration,
        .input_pixel_format              = status.current_input_pixel_format,
    };
}

class node_impl : public node_i
{
    std::unique_ptr<input_capture_s> capture_;

    std::shared_ptr<gpu::texture_s>                       framebuffer_;
    utils::observed_value_s<uint64_t>                     device_version_;
    utils::observed_value_s<std::pair<std::string, bool>> capture_selection_;
    utils::observed_value_s<BMDColorspace>                colorspace_;
    utils::observed_value_s<std::tuple<std::string, std::optional<bool>, std::optional<std::string>>>
        device_status_event_;

    gpu::color_conversion_s yuv_conversion_{};
    gpu::mat3               gamut_conversion_{1.0F};
    gpu::texture_frame_ptr  rendered_input_frame_;

    output_interface_s<const gpu::texture_s*> iface_tex_{*this, "tex"};

    void stop_capture()
    {
        if (capture_) {
            capture_->release_prepared_frame();
        }
        framebuffer_.reset();
        if (capture_) {
            capture_->reset_frames();
            capture_->stop_async();
            capture_ = nullptr;
        }
    }

    void publish_device_status(core::app_state_s* app, std::string_view device_name)
    {
        const auto device_status = app->decklink_registry()->get_device_status(device_name);
        auto       payload       = make_device_status(device_status ? *device_status : device_status_s{});
        const bool important     = device_status_event_.observe(
            std::tuple(std::string(device_name), payload.signal_locked, payload.active_format));
        app->status_registry()->write(status_handle_,
                                      std::move(payload),
                                      important ? core::status_delivery_e::immediate
                                                : core::status_delivery_e::rate_limited);
    }

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        if (!capture_) {
            return;
        }

        const auto metrics = capture_->metrics();
        status_registry->write(status_handle_,
                               status::decklink_input_metrics_status_s{
                                   .frames_received            = metrics.frames_received,
                                   .frames_missing             = metrics.frames_missing,
                                   .no_input_source_frames     = metrics.no_input_source_frames,
                                   .upload_slot_drops          = metrics.upload_slot_drops,
                                   .upload_acquire_slow_count  = metrics.upload_acquire_slow_count,
                                   .upload_acquire_failures    = metrics.upload_acquire_failures,
                                   .upload_acquire_wait_max_us = metrics.upload_acquire_wait_max_us,
                                   .content_frames_sampled     = metrics.content_frames_sampled,
                                   .content_frame_repeats      = metrics.content_frame_repeats,
                                   .content_repeat_streak      = metrics.content_repeat_streak,
                                   .content_repeat_streak_max  = metrics.content_repeat_streak_max,
                                   .available_video_frames     = metrics.available_video_frames,
                               });
        status_registry->write(
            status_handle_,
            status::source_timing_status_s{
                .source_queue_pushed                  = metrics.source_queue.pushed,
                .source_queue_depth                   = metrics.source_queue.queued,
                .source_queue_overflow_drops          = metrics.source_queue.overflow_drops,
                .source_queue_selection_drops         = metrics.source_queue.selection_drops,
                .source_queue_repeated                = metrics.source_queue.repeated,
                .source_queue_starvation_repeats      = metrics.source_queue.starvation_repeats,
                .source_queue_timing_repeats          = metrics.source_queue.timing_repeats,
                .source_queue_missing                 = metrics.source_queue.missing,
                .source_queue_discontinuities         = metrics.source_queue.discontinuities,
                .source_queue_transfer_failures       = metrics.source_queue.transfer_failures,
                .source_queue_transfer_cancellations  = metrics.source_queue.transfer_cancellations,
                .source_recovered_rate                = metrics.source_queue.recovered_rate,
                .source_observed_rate                 = metrics.source_queue.observed_rate,
                .source_phase_offset_us               = metrics.source_queue.phase_offset,
                .source_phase_error_us                = metrics.source_queue.phase_error,
                .source_phase_adjustment_us           = metrics.source_queue.phase_adjustment,
                .source_repeat_next_frame_lead_min_us = metrics.source_queue.repeat_next_frame_lead_min,
                .source_repeat_next_frame_lead_max_us = metrics.source_queue.repeat_next_frame_lead_max,
            });
    }

    void prepare_active_capture(core::app_state_s* app, core::node_status_registry_s* status_registry)
    {
        if (!capture_) {
            return;
        }

        if (capture_->requires_render_release()) {
            capture_->reset_frames();
            capture_->acknowledge_render_release();
        }

        const auto phase = capture_->phase();
        if (phase == input_capture_s::phase_e::failed) {
            log()->error("DeckLink input capture failed");
            stop_capture();
            capture_selection_.reset();
            report_connection(status_registry, {.connected = false});
            return;
        }
        if (phase == input_capture_s::phase_e::stopped) {
            capture_ = nullptr;
            capture_selection_.reset();
            report_connection(status_registry, {.connected = false});
            return;
        }

        if (phase == input_capture_s::phase_e::running) {
            const auto& frame = app->frame_context();
            capture_->advance_frames(frame.program_pts, frame.program_target_time, frame.discontinuity);
        }
    }

    bool start_capture(core::app_state_s* app, decklink_ptr<IDeckLinkInput> device, std::string_view device_name)
    {
        auto reservation = device_reservation_s<IDeckLinkInput>::acquire(device.get());
        if (!reservation) {
            return false;
        }

        log()->info("Scheduling DeckLink input setup for {}", device_name);
        capture_ = std::make_unique<input_capture_s>(app->texture_upload_service(),
                                                     app->decklink_registry()->control_executor(),
                                                     std::move(device),
                                                     std::move(reservation),
                                                     std::string(device_name));
        capture_->start_async();
        return true;
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override { stop_capture(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        auto* sr = app->status_registry();

        const auto current_version     = app->decklink_registry()->get_device_list_version();
        const bool device_list_changed = device_version_.observe(current_version);
        if (device_list_changed) {
            sr->write(status_handle_,
                      status::device_names_status_s{.device_names = app->decklink_registry()->get_input_options()},
                      core::status_delivery_e::immediate);
        }

        prepare_active_capture(app, sr);

        auto device_name = state.get_option<std::string>("device_name");
        auto enabled     = state.get_option<bool>("enabled");

        if (device_list_changed && capture_ && !app->decklink_registry()->get_input(device_name)) {
            stop_capture();
            capture_selection_.reset();
        }

        publish_device_status(app, device_name);
        publish_metrics(sr);

        const auto selection = std::pair(device_name, enabled);
        if (capture_selection_.would_change(selection)) {
            stop_capture();

            if (!enabled) {
                capture_selection_.commit(selection);
                report_connection(sr, {.connected = false});
                return;
            }

            auto device = app->decklink_registry()->get_input(device_name);
            if (!device) {
                report_connection(sr, {.connected = false});
                return;
            }

            if (!start_capture(app, std::move(device), device_name)) {
                report_connection(sr, {.connected = false});
                return;
            }
            capture_selection_.commit(selection);
        }

        report_connection(sr, {.connected = capture_ && capture_->phase() == input_capture_s::phase_e::running});
    }

    void submit(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (capture_) {
            const auto& frame = app->frame_context();
            (void)capture_->submit_frame(frame.program_pts);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        rendered_input_frame_.reset();
        // Let recorded graph work run while waiting for the exact PTS-selected upload.
        app->submit_gpu();
        const auto frame = capture_ ? capture_->resolve_frame() : std::nullopt;
        if (!frame.has_value()) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_.get() : nullptr);
            return;
        }
        rendered_input_frame_ = frame->frame;
        app->commands().wait_for(rendered_input_frame_->upload_completion());

        framebuffer_ = rendered_input_frame_->conversion_texture();

        if (colorspace_.observe(frame->colorspace)) {
            const auto transfer = get_color_transfer(colorspace_.value());
            yuv_conversion_     = gpu::get_color_transfer_from_yuv(transfer);
            gamut_conversion_   = gpu::get_gamut_transfer_to_rec709(transfer);
        }

        framebuffer_->clear(app->commands());
        const auto& source = *rendered_input_frame_;
        app->commands().unpack_v210(
            source.buffer(),
            *framebuffer_,
            gpu::color_parameters(yuv_conversion_, gamut_conversion_, gpu::color_conversion_direction_e::from_yuv),
            source.layout().row_stride_bytes);

        auto fb_tex = framebuffer_.get();
        app->commands().generate_mip_maps(*fb_tex);
        iface_tex_.set_value(fb_tex);
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (rendered_input_frame_) {
            rendered_input_frame_.reset();
        }
        if (capture_) {
            capture_->release_prepared_frame();
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",    "DeckLink input"},
            {"enabled", true            },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "decklink_input"; }
};
} // namespace

namespace miximus::nodes::decklink {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::decklink
