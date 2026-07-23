#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
#include "detail/device_reservation.hpp"
#include "detail/input_capture.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/observed_value.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::decklink_sdk;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace miximus::nodes::decklink::detail;

auto log() { return getlog("decklink"); }

template <typename T>
nlohmann::json status_value(const std::optional<T>& value)
{
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

void write_device_status(core::node_status_registry_s::writer_s& writer, const device_status_s& status)
{
    writer.write("signal_locked", status_value(status.input_signal_locked));
    writer.write("ancillary_signal_locked", status_value(status.ancillary_signal_locked));
    writer.write("capture_busy", status_value(status.capture_busy));
    writer.write("pcie_link_width", status_value(status.pcie_link_width));
    writer.write("pcie_link_speed", status_value(status.pcie_link_speed));
    writer.write("temperature_c", status_value(status.temperature_c));
    writer.write("active_format", status_value(status.current_input_mode));
    writer.write("detected_format", status_value(status.detected_input_mode));
    writer.write("detected_colorspace", status_value(status.detected_colorspace));
    writer.write("detected_dynamic_range", status_value(status.detected_dynamic_range));
    writer.write("detected_field_dominance", status_value(status.detected_field_dominance));
    writer.write("detected_sdi_link_configuration", status_value(status.detected_sdi_link_configuration));
    writer.write("input_pixel_format", status_value(status.current_input_pixel_format));
}

class node_impl : public node_i
{
    std::unique_ptr<input_capture_s> capture_;

    std::unique_ptr<gpu::framebuffer_s>                       framebuffer_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_;
    utils::observed_value_s<uint64_t>                         device_version_;
    utils::observed_value_s<std::pair<std::string, bool>>     capture_selection_;
    utils::observed_value_s<BMDColorspace>                    colorspace_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> device_status_version_;
    std::chrono::steady_clock::time_point                     next_metrics_status_;
    gpu::color_conversion_s                                   yuv_conversion_{};
    gpu::mat3                                                 gamut_conversion_{1.0F};

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

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
        const auto status_key    = std::pair(std::string(device_name), device_status ? device_status->version : 0);
        if (device_status_version_.observe(status_key)) {
            auto writer = app->status_registry()->write_node(id_);
            write_device_status(writer, device_status ? *device_status : device_status_s{});
        }
    }

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!capture_ || now < next_metrics_status_) {
            return;
        }

        const auto metrics = capture_->metrics();
        auto       writer  = status_registry->write_node(id_);
        writer.write("frames_received", metrics.frames_received);
        writer.write("frames_missing", metrics.frames_missing);
        writer.write("no_input_source_frames", metrics.no_input_source_frames);
        writer.write("upload_slot_drops", metrics.upload_slot_drops);
        writer.write("available_video_frames", metrics.available_video_frames);
        writer.write("source_queue_pushed", metrics.source_queue.pushed);
        writer.write("source_queue_overflow_drops", metrics.source_queue.overflow_drops);
        writer.write("source_queue_selection_drops", metrics.source_queue.selection_drops);
        writer.write("source_queue_repeated", metrics.source_queue.repeated);
        writer.write("source_queue_missing", metrics.source_queue.missing);
        writer.write("source_queue_discontinuities", metrics.source_queue.discontinuities);
        writer.write("source_queue_transfer_failures", metrics.source_queue.transfer_failures);
        writer.write("source_recovered_rate", status_value(metrics.source_queue.recovered_rate));
        writer.write(
            "source_phase_offset_us",
            metrics.source_queue.phase_offset.has_value()
                ? nlohmann::json(
                      std::chrono::duration_cast<std::chrono::microseconds>(*metrics.source_queue.phase_offset).count())
                : nlohmann::json(nullptr));
        next_metrics_status_ = now + std::chrono::seconds(1);
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
            status_registry->write(id_, "connected", false);
            return;
        }
        if (phase == input_capture_s::phase_e::stopped) {
            capture_ = nullptr;
            capture_selection_.reset();
            status_registry->write(id_, "connected", false);
            return;
        }

        if (phase == input_capture_s::phase_e::running) {
            const auto& frame = app->frame_context();
            capture_->advance_frames(frame.pts, frame.target_time, frame.discontinuity);
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
            sr->write(id_, "device_names", app->decklink_registry()->get_input_options());
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
                sr->write(id_, "connected", false);
                return;
            }

            auto device = app->decklink_registry()->get_input(device_name);
            if (!device) {
                sr->write(id_, "connected", false);
                return;
            }

            if (!start_capture(app, std::move(device), device_name)) {
                sr->write(id_, "connected", false);
                return;
            }
            capture_selection_.commit(selection);
        }

        sr->write(id_, "connected", capture_ && capture_->phase() == input_capture_s::phase_e::running);
    }

    void submit(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (capture_) {
            (void)capture_->submit_frame(app->frame_context().pts);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        const auto frame = capture_ ? capture_->resolve_frame() : std::nullopt;
        if (!frame.has_value()) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::yuv_to_rgb);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        const auto src_dim = frame->dimensions;

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != src_dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(src_dim, gpu::texture_s::format_e::rgb_f16);
        }

        auto shader = textured_quad_->shader();
        shader->set_uniform("target_width", src_dim.x);

        if (colorspace_.observe(frame->colorspace)) {
            const auto transfer = get_color_transfer(colorspace_.value());
            yuv_conversion_     = gpu::get_color_transfer_from_yuv(transfer);
            gamut_conversion_   = gpu::get_gamut_transfer_to_rec709(transfer);
        }

        shader->set_uniform("transfer", yuv_conversion_.matrix);
        shader->set_uniform("transfer_offset", yuv_conversion_.offset);
        shader->set_uniform("gamut_transfer", gamut_conversion_);

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(frame->texture);
        gpu::framebuffer_s::end_render();

        auto fb_tex = framebuffer_->texture();
        fb_tex->generate_mip_maps();
        iface_tex_.set_value(fb_tex);
    }

    void complete(core::app_state_s* /*app*/) final
    {
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
