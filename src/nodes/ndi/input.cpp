#include "core/node_status_registry.hpp"
#include "detail/input_capture.hpp"
#include "gpu/drawing.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"
#include "media/media_clock_sample.hpp"
#include "nodes/execution_context.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "nodes/source_timing_status.hpp"
#include "registry.hpp"
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::ndi;
using namespace miximus::nodes::ndi::detail;

auto log() { return getlog("ndi"); }

struct capture_selection_s
{
    std::string source_name;
    bool        enabled;
    int         buffer_frames;
    bool        operator==(const capture_selection_s&) const = default;
};

class node_impl : public node_i
{
    std::shared_ptr<input_capture_s> capture_;

    std::shared_ptr<gpu::texture_s> framebuffer_;

    utils::observed_value_s<uint64_t>            source_version_;
    utils::observed_value_s<capture_selection_s> capture_selection_;

    gpu::texture_frame_ptr rendered_input_frame_;

    output_interface_s<texture_source_info_s> iface_tex_{*this, "tex"};

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
        if (!capture_) {
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
        status_registry->write(id_, source_timing_status(metrics.source_queue));
    }

    void update_capture_lifecycle(execution_context_s*          context,
                                  core::node_status_registry_s* status_registry,
                                  const capture_selection_s&    selection)
    {
        if (capture_) {
            const auto phase = capture_->phase();
            if (phase == input_capture_s::phase_e::failed || phase == input_capture_s::phase_e::stopped) {
                if (phase == input_capture_s::phase_e::failed) {
                    log()->error("NDI input capture failed for \"{}\"", selection.source_name);
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
        if (!selection.enabled || selection.source_name.empty()) {
            capture_selection_.commit(selection);
            report_connection(status_registry, {.connected = false});
            return;
        }

        log()->info("Scheduling NDI input setup for \"{}\"", selection.source_name);
        capture_ = input_capture_s::create(context->texture_upload_service(),
                                           context->ndi_registry()->control_executor(),
                                           selection.source_name,
                                           id_,
                                           static_cast<size_t>(selection.buffer_frames));
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

    void prepare(execution_context_s* context, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        auto* status_registry = context->status_registry();

        const auto current_version = context->ndi_registry()->get_source_list_version();
        if (source_version_.observe(current_version)) {
            status_registry->write(
                id_,
                status::source_names_status_s{.source_names = context->ndi_registry()->get_source_options()},
                core::status_delivery_e::immediate);
        }

        const capture_selection_s selection{.source_name   = state.get_option<std::string>("source_name"),
                                            .enabled       = state.get_option<bool>("enabled"),
                                            .buffer_frames = context->frame_settings().ndi_input.buffer_frames};
        update_capture_lifecycle(context, status_registry, selection);
        publish_metrics(status_registry);

        if (capture_ && capture_->phase() == input_capture_s::phase_e::running) {
            const auto& frame = context->frame_context();
            capture_->advance_frames(frame.program_pts, frame.program_target_time, frame.discontinuity);
        }

        report_connection(status_registry,
                          {.connected = capture_ && capture_->phase() == input_capture_s::phase_e::running});
    }

    void submit(execution_context_s* context, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (capture_) {
            const auto& frame = context->frame_context();
            (void)capture_->submit_frame(frame.program_pts);
        }
    }

    void execute(execution_context_s* context, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        rendered_input_frame_.reset();
        // Let recorded graph work run while waiting for the exact PTS-selected upload.
        context->submit_gpu();
        const auto frame = capture_ ? capture_->resolve_frame() : std::nullopt;
        if (!frame.has_value()) {
            iface_tex_.set_value({.texture = framebuffer_.get(), .name = state.get_option_string_view("name")});
            return;
        }
        rendered_input_frame_ = frame->frame;
        context->commands().wait_for(rendered_input_frame_->upload_completion());

        framebuffer_ = rendered_input_frame_->conversion_texture();

        // The shared timed-source queue has already selected and aligned this
        // raw NDI frame. Interpret alpha as configured, then convert into linear
        // premultiplied working RGB before downstream filtering/compositing.

        framebuffer_->clear(context->commands());
        gpu::draw_texture(context->commands(),
                          rendered_input_frame_->texture(),
                          framebuffer_.get(),
                          {.transfer = gpu::rec709_decode_operation(
                               state.get_enum_option_unchecked<gpu::input_alpha_mode_e>("alpha_mode")),
                           .compositing = gpu::compositing_e::replace});

        auto* output = framebuffer_.get();
        context->commands().generate_mip_maps(*output);
        iface_tex_.set_value({.texture = output, .name = state.get_option_string_view("name")});
    }

    void complete(execution_context_s* /*context*/) final
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
            {"name",        "NDI Input"                                      },
            {"alpha_mode",  enum_to_string(gpu::input_alpha_mode_e::straight)},
            {"enabled",     true                                             },
            {"source_name", ""                                               },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "alpha_mode") {
            return normalize_enum_option_value<gpu::input_alpha_mode_e>(value);
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
