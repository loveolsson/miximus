#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/input_capture.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "logger/logger.hpp"
#include "media/media_frame.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
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

    std::unique_ptr<gpu::framebuffer_s>   framebuffer_;
    std::unique_ptr<gpu::textured_quad_s> textured_quad_;

    utils::observed_value_s<uint64_t>                     source_version_;
    utils::observed_value_s<std::pair<std::string, bool>> capture_selection_;
    std::chrono::steady_clock::time_point                 next_metrics_status_;

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

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
        auto       writer  = status_registry->write_node(id_);
        writer.write("frames_received", metrics.frames_received);
        writer.write("invalid_frames", metrics.invalid_frames);
        writer.write("receiver_video_drops", metrics.receiver_video_drops);
        writer.write("receiver_queue_depth", metrics.receiver_queue_depth);
        writer.write("upload_slot_drops", metrics.upload_slot_drops);
        writer.write("source_queue_pushed", metrics.source_queue.pushed);
        writer.write("source_queue_depth", metrics.source_queue.queued);
        writer.write("source_queue_overflow_drops", metrics.source_queue.overflow_drops);
        writer.write("source_queue_selection_drops", metrics.source_queue.selection_drops);
        writer.write("source_queue_repeated", metrics.source_queue.repeated);
        writer.write("source_queue_missing", metrics.source_queue.missing);
        writer.write("source_queue_discontinuities", metrics.source_queue.discontinuities);
        writer.write("source_queue_transfer_failures", metrics.source_queue.transfer_failures);
        if (metrics.source_queue.recovered_rate.has_value()) {
            writer.write("source_recovered_rate", *metrics.source_queue.recovered_rate);
        }
        if (metrics.source_queue.phase_offset.has_value()) {
            writer.write("source_phase_offset_us", metrics.source_queue.phase_offset->count() / 706);
        }
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
            status_registry->write(id_, "connected", false);
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
            status_registry->write(id_, "source_names", app->ndi_registry()->get_source_options());
        }

        const auto selection =
            std::pair(state.get_option<std::string>("source_name"), state.get_option<bool>("enabled"));
        update_capture_lifecycle(app, status_registry, selection);
        publish_metrics(status_registry);

        if (capture_ && capture_->phase() == input_capture_s::phase_e::running) {
            const auto& frame = app->frame_context();
            capture_->advance_frames(frame.pts, frame.target_time, frame.discontinuity);
        }

        status_registry->write(id_, "connected", capture_ && capture_->phase() == input_capture_s::phase_e::running);
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

        if (!framebuffer_ || framebuffer_->texture()->display_dimensions() != frame->dimensions) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(frame->dimensions, gpu::texture_s::format_e::rgb_f16);
        }

        // The shared timed-source queue has already selected and aligned this
        // raw NDI frame. This conversion only linearizes its Rec.709 image.
        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::strip_gamma);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(frame->texture);
        gpu::framebuffer_s::end_render();

        auto* output = framebuffer_->texture();
        output->generate_mip_maps();
        iface_tex_.set_value(output);
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
            {"name",        "NDI Input"},
            {"enabled",     true       },
            {"source_name", ""         },
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

    std::string_view type() const final { return "ndi_input"; }
};
} // namespace

namespace miximus::nodes::ndi {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::ndi
