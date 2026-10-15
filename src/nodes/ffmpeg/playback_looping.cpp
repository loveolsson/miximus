#include "core/node_status_registry.hpp"
#include "detail/playback.hpp"
#include "detail/playback_timeline.hpp"
#include "nodes/execution_context.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/observed_value.hpp"

#include <cstdint>

namespace miximus::nodes::ffmpeg {

namespace {

enum class playback_mode_e : uint8_t
{
    always_play,
    pause_inactivity,
    pause_and_rewind_inactive
};

class node_impl final : public node_i
{
    output_interface_s<texture_source_info_s>           texture_{*this, "texture"};
    ffmpeg_registry_s*                                  registry_{};
    std::shared_ptr<detail::playback_session_s>         session_;
    utils::observed_value_s<detail::playback_options_s> selection_;
    detail::playback_timeline_s                         timeline_;
    playback_mode_e                                     mode_{playback_mode_e::always_play};
    bool                                                executed_{};
    bool                                                previously_active_{};
    std::shared_ptr<const detail::playback_frame_s>     prepared_;
    std::shared_ptr<const detail::playback_frame_s>     displayed_;
    gpu::texture_frame_ptr                              frame_;
    std::shared_ptr<gpu::texture_s>                     output_;

    void stop()
    {
        prepared_.reset();
        displayed_.reset();
        frame_.reset();
        output_.reset();
        if (session_) {
            registry_->retire(std::move(session_));
        }
    }

  public:
    node_impl()                                  = default;
    node_impl(const node_impl& other)            = delete;
    node_impl& operator=(const node_impl& other) = delete;
    node_impl(node_impl&& other)                 = delete;
    node_impl& operator=(node_impl&& other)      = delete;

    ~node_impl() override { stop(); }

    void prepare(execution_context_s* context, const node_state_s& state, prepare_result_s* /*result*/) final
    {
        executed_ = false;
        registry_ = context->ffmpeg_registry();
        mode_     = state.get_enum_option_unchecked<playback_mode_e>("playback_mode");
        detail::playback_options_s options{
            .filename = state.get_option<bool>("enabled") ? state.get_option<std::string>("filename") : std::string{},
            .attempt_hardware_decode = state.get_option<bool>("attempt_hardware_decode"),
            .buffered_frames         = context->frame_settings().ffmpeg.buffer_frames,
        };

        if (selection_.observe(options)) {
            stop();
            timeline_          = {};
            previously_active_ = false;
            if (!options.filename.empty()) {
                session_ = registry_->create(std::move(options));
            }
        }

        detail::playback_status_s status;
        if (session_) {
            status             = session_->status();
            const auto& timing = context->frame_context();
            if (mode_ == playback_mode_e::always_play) {
                timeline_.tick(timing.program_pts, timing.epoch, status.ready);
            }

            session_->advance(timeline_.position());
        }

        report_connection(context->status_registry(), {.connected = status.ready && status.error.empty()});
        context->status_registry()->write(
            id_,
            status::ffmpeg_playback_status_s{
                .playback_backend          = status.backend,
                .playback_transfer_path    = status.transfer_path,
                .playback_fallback_reason  = status.fallback_reason,
                .playback_error            = status.error,
                .playback_position_seconds = std::chrono::duration<double>(timeline_.position()).count(),
                .playback_decoded_frames   = status.decoded_frames,
                .playback_dropped_frames   = status.dropped_frames,
                .playback_loops            = status.loops,
                .playback_queue_depth      = status.queued_frames,
            });
    }

    void submit(execution_context_s* /*context*/, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        prepared_ = session_ ? session_->selected() : nullptr;
    }

    void execute(execution_context_s* context, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        executed_ = true;
        if (session_ && mode_ != playback_mode_e::always_play) {
            const auto& timing = context->frame_context();
            timeline_.tick(timing.program_pts, timing.epoch, session_->status().ready);
            session_->advance(timeline_.position());
            prepared_ = session_->selected();
        }

        if (prepared_ && prepared_ != displayed_) {
            context->submit_gpu();
            auto& source = *prepared_;
            if (source.gpu_texture) {
                output_    = source.gpu_texture;
                displayed_ = prepared_;
                frame_.reset();
            } else if (source.stream->wait_for_upload_submission(source.upload_id) ==
                       gpu::transfer::texture_upload_wait_result_e::ready) {
                auto next = source.stream->select_submitted_upload(source.upload_id);
                if (next) {
                    output_    = next->conversion_texture();
                    frame_     = std::move(next);
                    displayed_ = prepared_;
                }
            }
        }

        // Re-record on repeats as the graph may have aborted the previous frame.
        // CPU selection is not evidence that its conversion was submitted.
        if (frame_ && displayed_) {
            context->commands().wait_for(frame_->upload_completion());
            context->commands().unpack_video(frame_->buffer(), *output_, displayed_->color);
            context->commands().generate_mip_maps(*output_);
        }

        texture_.set_value({.texture = output_.get(), .name = state.get_option_string_view("name")});
    }

    void complete(execution_context_s* context) final
    {
        prepared_.reset();
        if (mode_ != playback_mode_e::always_play && !executed_) {
            const auto& timing = context->frame_context();
            timeline_.tick(timing.program_pts, timing.epoch, false);
            if (previously_active_ && mode_ == playback_mode_e::pause_and_rewind_inactive) {
                timeline_.rewind();
                if (session_) {
                    session_->rewind();
                }

                displayed_.reset();
                frame_.reset();
                output_.reset();
            }
        }

        previously_active_ = executed_;
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",                    "Video playback (looping)"},
            {"enabled",                 true                      },
            {"filename",                ""                        },
            {"playback_mode",           "always_play"             },
            {"attempt_hardware_decode", true                      }
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "filename") {
            return normalize_option_value<std::string_view>(value);
        }

        if (name == "playback_mode") {
            return normalize_enum_option_value<playback_mode_e>(value);
        }

        if (name == "enabled" || name == "attempt_hardware_decode") {
            return normalize_option_value<bool>(value);
        }

        return option_result_e::invalid;
    }

    std::optional<utils::file_dialog_request_s> file_dialog_request(std::string_view    field,
                                                                    const node_state_s& state) const final
    {
        if (field != "filename") {
            return std::nullopt;
        }

        return utils::file_dialog_request_s{
            .title        = "Miximus - Select video",
            .initial_path = state.get_option<std::string>(field),
            .filters      = {{.label = "Video", .pattern = "*.mp4;*.mov;*.mkv;*.avi;*.mxf;*.webm;*.ts;*.mpg"},
                             {.label = "All files", .pattern = "*.*"}}
        };
    }

    std::string_view type() const final { return "ffmpeg_playback_looping"; }
};

} // namespace

std::shared_ptr<node_i> create_playback_looping_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::ffmpeg
