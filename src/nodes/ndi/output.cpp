#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/output_sender.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "logger/logger.hpp"
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
#include <tuple>
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
    using timing_selection_t = std::tuple<frame_rate_s, uint64_t, int>;

    std::shared_ptr<output_sender_s>                          sender_;
    std::shared_ptr<gpu::transfer::texture_readback_stream_s> readback_stream_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_;

    utils::observed_value_s<std::pair<std::string, bool>>   sender_selection_;
    utils::observed_value_s<timing_selection_t>             timing_selection_;
    utils::observed_value_s<gpu::vec2i_t>                   stream_dimensions_;
    std::chrono::steady_clock::time_point                   next_metrics_status_;
    uint64_t                                                render_target_drops_{};
    std::optional<gpu::transfer::texture_readback_target_s> render_target_;

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    void clear_render_state()
    {
        render_target_.reset();
        if (sender_) {
            sender_->clear_stream();
        }
        readback_stream_.reset();
        stream_dimensions_.reset();
        textured_quad_.reset();
    }

    void stop_sender()
    {
        clear_render_state();
        if (sender_) {
            sender_->stop_async();
            sender_.reset();
        }
    }

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!sender_ || now < next_metrics_status_) {
            return;
        }

        const auto metrics       = sender_->metrics();
        const auto output_status = status::ndi_output_metrics_status_s{
            .program_frames_received      = metrics.program_frames_received,
            .program_queue_overflow_drops = metrics.program_queue_overflow_drops,
            .program_timing_drops         = metrics.program_timing_drops,
            .program_frames_repeated      = metrics.program_frames_repeated,
            .program_frames_missing       = metrics.program_frames_missing,
            .output_intervals_skipped     = metrics.output_intervals_skipped,
            .frames_sent                  = metrics.frames_sent,
            .queued_frames                = metrics.queued_frames,
            .output_latency_us            = metrics.output_latency_us,
            .program_selection_offset_us  = metrics.program_selection_offset_us,
            .render_target_drops          = render_target_drops_,
        };
        if (readback_stream_) {
            const auto readback_metrics = readback_stream_->metrics();
            status_registry->write(id_, output_status);
            status_registry->write(
                id_,
                status::download_stream_status_s{
                    .download_slots                      = readback_metrics.slots,
                    .download_slots_free                 = readback_metrics.free_slots,
                    .download_slots_rendering            = readback_metrics.rendering_slots,
                    .download_slots_queued               = readback_metrics.queued_slots,
                    .download_slots_ready                = readback_metrics.ready_slots,
                    .download_slots_cpu_reading          = readback_metrics.cpu_reading_slots,
                    .download_pending_allocations        = readback_metrics.pending_allocations,
                    .download_acquire_misses             = readback_metrics.render_target_acquire_misses,
                    .download_transfers_completed        = readback_metrics.transfers_completed,
                    .download_transfer_failures          = readback_metrics.transfer_failures,
                    .download_transfer_duration_total_us = readback_metrics.transfer_duration_total_us,
                    .download_transfer_duration_max_us   = readback_metrics.transfer_duration_max_us,
                    .download_allocation_failed          = readback_metrics.allocation_failed,
                });
        } else {
            status_registry->write(id_, output_status);
        }
        next_metrics_status_ = now + 1s;
    }

    void update_sender_lifecycle(core::app_state_s*                  app,
                                 core::node_status_registry_s*       status_registry,
                                 const std::pair<std::string, bool>& selection)
    {
        if (sender_) {
            const auto phase = sender_->phase();
            if (phase == output_sender_s::phase_e::failed || phase == output_sender_s::phase_e::stopped) {
                if (phase == output_sender_s::phase_e::failed) {
                    log()->error("NDI output sender failed for \"{}\"", selection.first);
                    sender_->stop_async();
                }
                sender_.reset();
                sender_selection_.reset();
            }
        }

        if (!sender_selection_.would_change(selection)) {
            return;
        }

        stop_sender();
        render_target_drops_ = 0;
        if (!selection.second) {
            sender_selection_.commit(selection);
            status_registry->write(id_, status::connected_status_s{.connected = false});
            return;
        }

        log()->info("Scheduling NDI output setup for \"{}\"", selection.first);
        sender_ = output_sender_s::create(app->ndi_registry()->control_executor(), selection.first);
        sender_selection_.commit(selection);
        sender_->start_async();
    }

    void ensure_readback_stream(core::app_state_s* app, gpu::vec2i_t dimensions)
    {
        const auto& settings = app->frame_settings();
        if (readback_stream_ && !stream_dimensions_.would_change(dimensions)) {
            return;
        }

        clear_render_state();
        const gpu::transfer::texture_transfer_layout_s transfer_layout{
            .dimensions             = dimensions,
            .pixel_format           = gpu::texture_s::pixel_format_e::rgba_u8,
            .host_row_stride_bytes  = static_cast<size_t>(dimensions.x) * 4,
            .host_buffer_size_bytes = static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y) * 4,
            .host_memory_access     = gpu::transfer::host_memory_access_e::read_only,
        };
        const auto readback_slot_count =
            output_sender_s::get_readback_slot_count(static_cast<size_t>(settings.ndi_output.buffer_frames));
        readback_stream_ = app->texture_readback_service()->create_stream({
            .transfer_layout = transfer_layout,
            .max_slots       = readback_slot_count,
            .initial_slots   = readback_slot_count,
        });
        stream_dimensions_.commit(dimensions);
        sender_->set_stream(readback_stream_,
                            dimensions,
                            settings.frame_rate,
                            app->frame_context().frame_duration,
                            app->frame_context().program_target_time - app->frame_context().program_pts,
                            static_cast<size_t>(settings.ndi_output.buffer_frames));
    }

  public:
    ~node_impl() override { stop_sender(); }

    node_impl()                            = default;
    node_impl(const node_impl&)            = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* result) final
    {
        auto*      status_registry = app->status_registry();
        const auto enabled         = state.get_option<bool>("enabled");
        const auto configured_name = state.get_option<std::string>("source_name", id_);
        const auto sender_name     = configured_name.empty() ? id_ : configured_name;
        const auto frame_rate      = app->frame_settings().frame_rate;
        const auto frame_rate_valid =
            std::in_range<int>(frame_rate.numerator) && std::in_range<int>(frame_rate.denominator);
        result->demands_execution = enabled && frame_rate_valid;

        if (!frame_rate_valid) {
            log()->error("Cannot represent the configured frame rate in the NDI API");
        }

        update_sender_lifecycle(app, status_registry, std::pair(sender_name, enabled && frame_rate_valid));

        const auto timing = timing_selection_t{
            app->frame_settings().frame_rate,
            app->frame_context().epoch,
            app->frame_settings().ndi_output.buffer_frames,
        };
        if (timing_selection_.observe(timing)) {
            clear_render_state();
        }

        publish_metrics(status_registry);
        status_registry->write(id_,
                               status::connected_status_s{
                                   .connected = sender_ && sender_->phase() == output_sender_s::phase_e::running,
                               });
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        render_target_.reset();
        if (!sender_ || sender_->phase() != output_sender_s::phase_e::running) {
            return;
        }

        const auto texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr) {
            return;
        }

        ensure_readback_stream(app, texture->display_dimensions());
        if (readback_stream_->initial_slots_pending()) {
            return;
        }
        auto target = readback_stream_->try_acquire_render_target();
        if (!target.has_value()) {
            ++render_target_drops_;
            return;
        }

        // Convert the internal linear image to the Rec.709 RGBA representation
        // advertised in the NDI metadata, with top-to-bottom row order.
        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::apply_gamma);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        target->framebuffer()->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(texture);
        gpu::framebuffer_s::end_render();

        target->set_program_target_time(app->frame_context().program_target_time);
        render_target_ = std::move(target);
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (render_target_) {
            render_target_->submit();
            render_target_.reset();
        }
        if (sender_) {
            sender_->notify_frame();
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",        "NDI Output"},
            {"enabled",     true        },
            {"source_name", id_         },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "source_name") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "ndi_output"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<node_i> create_output_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
