#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/input_capture.hpp"
#include "gpu/drawing.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"
#include "media/media_clock_sample.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace std::chrono_literals;
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::ndi;
using namespace miximus::nodes::ndi::detail;

auto log() { return getlog("ndi"); }

class node_impl : public node_i
{
    std::shared_ptr<input_capture_s> capture_;

    std::shared_ptr<gpu::texture_s> framebuffer_;

    utils::observed_value_s<uint64_t>                     source_version_;
    utils::observed_value_s<std::pair<std::string, bool>> capture_selection_;
    std::chrono::steady_clock::time_point                 next_metrics_status_;
    gpu::texture_frame_ptr                                rendered_input_frame_;

    output_interface_s<const gpu::texture_s*> iface_tex_{*this, "tex"};

    void stop_capture()
    {
        framebuffer_.reset();
        if (capture_) {
            capture_->reset_frames();
            capture_->stop_async();
            capture_.reset();
        }
    }

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!capture_ || now < next_metrics_status_) {
            return;
        }

        const auto metrics = capture_->metrics();
        status_registry->write(id_,
                               status::ndi_input_metrics_status_s{
                                   .frames_received      = metrics.frames_received,
                                   .invalid_frames       = metrics.invalid_frames,
                                   .receiver_video_drops = metrics.receiver_video_drops,
                                   .receiver_queue_depth = metrics.receiver_queue_depth,
                                   .upload_slot_drops    = metrics.upload_slot_drops,
                               });
        status_registry->write(
            id_,
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
        next_metrics_status_ = now + 1s;
    }

    void update_capture_lifecycle(core::app_state_s*                  app,
                                  core::node_status_registry_s*       status_registry,
                                  const std::pair<std::string, bool>& selection)
    {
        if (capture_) {
            const auto phase = capture_->phase();
            if (phase == input_capture_s::phase_e::failed || phase == input_capture_s::phase_e::stopped) {
                if (phase == input_capture_s::phase_e::failed) {
                    log()->error("NDI input capture failed for \"{}\"", selection.first);
                    capture_->stop_async();
                }
                capture_.reset();
                capture_selection_.reset();
            }
        }

        if (!capture_selection_.would_change(selection)) {
            return;
        }

        stop_capture();
        if (!selection.second || selection.first.empty()) {
            capture_selection_.commit(selection);
            status_registry->write(id_, status::connected_status_s{.connected = false});
            return;
        }

        log()->info("Scheduling NDI input setup for \"{}\"", selection.first);
        capture_ = input_capture_s::create(
            app->texture_upload_service(), app->ndi_registry()->control_executor(), selection.first, id_);
        capture_selection_.commit(selection);
        capture_->start_async();
    }

  public:
    ~node_impl() override { stop_capture(); }

    node_impl()                            = default;
    node_impl(const node_impl&)            = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        auto* status_registry = app->status_registry();

        const auto current_version = app->ndi_registry()->get_source_list_version();
        if (source_version_.observe(current_version)) {
            status_registry->write(
                id_, status::source_names_status_s{.source_names = app->ndi_registry()->get_source_options()});
        }

        const auto selection =
            std::pair(state.get_option<std::string>("source_name"), state.get_option<bool>("enabled"));
        update_capture_lifecycle(app, status_registry, selection);
        publish_metrics(status_registry);

        if (capture_ && capture_->phase() == input_capture_s::phase_e::running) {
            const auto& frame = app->frame_context();
            capture_->advance_frames(frame.program_pts, frame.program_target_time, frame.discontinuity);
        }

        status_registry->write(id_,
                               status::connected_status_s{
                                   .connected = capture_ && capture_->phase() == input_capture_s::phase_e::running,
                               });
    }

    void submit(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (capture_) {
            const auto& frame = app->frame_context();
            (void)capture_->submit_frame(frame.program_pts);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& state) final
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

        // The shared timed-source queue has already selected and aligned this
        // raw NDI frame. Interpret alpha as configured, then convert into linear
        // premultiplied working RGB before downstream filtering/compositing.

        framebuffer_->clear(app->commands());
        gpu::draw_texture(
            app->commands(),
            rendered_input_frame_->texture(),
            framebuffer_.get(),
            {.transfer = gpu::rec709_decode_operation(state.get_enum_option_unchecked<gpu::alpha_mode_e>("alpha_mode")),
             .compositing = gpu::compositing_e::replace});

        auto* output = framebuffer_.get();
        app->commands().generate_mip_maps(*output);
        iface_tex_.set_value(output);
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
            {"name",        "NDI Input"                                },
            {"alpha_mode",  enum_to_string(gpu::alpha_mode_e::straight)},
            {"enabled",     true                                       },
            {"source_name", ""                                         },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "alpha_mode") {
            return normalize_enum_option_value<gpu::alpha_mode_e>(value);
        }

        if (name == "source_name") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "ndi_input"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
