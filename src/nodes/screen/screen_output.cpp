#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/output_presenter.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/geometry.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "types/node_status_json.hpp"
#include "utils/observed_value.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {
using namespace std::chrono_literals;
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::screen::detail;

class node_impl : public node_i
{
    using presenter_settings_t = std::tuple<bool, int, utils::flicks>;

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    std::unique_ptr<output_presenter_s>           presenter_;
    std::unique_ptr<gpu::textured_quad_s>         textured_quad_;
    utils::observed_value_s<uint64_t>             monitor_version_;
    utils::observed_value_s<gpu::recti_s>         window_rect_;
    utils::observed_value_s<bool>                 fullscreen_;
    utils::observed_value_s<std::string>          monitor_id_;
    utils::observed_value_s<presenter_settings_t> presenter_settings_;
    bool                                          presenter_stopping_{};
    std::chrono::steady_clock::time_point         next_metrics_status_;

    void destroy_presenter()
    {
        presenter_.reset();
        textured_quad_.reset();
        presenter_stopping_ = false;
    }

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!presenter_ || now < next_metrics_status_) {
            return;
        }

        const auto metrics = presenter_->metrics();
        status_registry->write(
            id_,
            status::screen_output_metrics_status_s{
                .clock_quality = metrics.uses_nominal_cadence ? "Nominal monitor cadence" : "Swap completion estimate",
                .frames_submitted             = metrics.frames_submitted,
                .program_queue_overflow_drops = metrics.program_queue_overflow_drops,
                .program_timing_drops         = metrics.program_timing_drops,
                .program_frames_repeated      = metrics.program_frames_repeated,
                .program_frames_missing       = metrics.program_frames_missing,
                .output_intervals_skipped     = metrics.output_intervals_skipped,
                .swaps_completed              = metrics.swaps_completed,
                .render_acquire_misses        = metrics.render_acquire_misses,
                .queued_frames                = metrics.queued_frames,
                .render_slots                 = metrics.slots,
                .render_slots_free            = metrics.free_slots,
                .render_slots_retiring        = metrics.retiring_slots,
                .output_latency_us            = metrics.output_latency_us,
                .program_selection_offset_us  = metrics.program_selection_offset_us,
                .completion_interval_max_us   = metrics.completion_interval_max_us,
                .measured_refresh_hz          = metrics.measured_refresh_hz,
            });
        next_metrics_status_ = now + 1s;
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override { destroy_presenter(); }

    node_impl(const node_impl&)            = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl& operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* result) final
    {
        const auto monitor_version = gpu::context_s::get_monitor_list_version();
        if (monitor_version_.observe(monitor_version)) {
            app->status_registry()->write(id_,
                                          status::monitor_options_status_s{.monitors = gpu::context_s::get_monitors()});
        }

        const auto enabled                = state.get_option<bool>("enabled", false);
        const auto monitor_id             = state.get_option<std::string>("monitor_id");
        auto       nominal_frame_duration = app->frame_context().frame_duration;
        if (const auto refresh_rate = gpu::context_s::get_monitor_refresh_rate(monitor_id); refresh_rate.has_value()) {
            nominal_frame_duration = utils::k_flicks_one_second / *refresh_rate;
        }
        const auto presenter_settings = presenter_settings_t{
            enabled,
            app->frame_settings().screen_output.buffer_frames,
            nominal_frame_duration,
        };
        const auto position                = state.get_option<gpu::vec2_t>("position", {0, 0});
        const auto size                    = state.get_option<gpu::vec2_t>("size", {100, 100});
        const auto rect                    = gpu::round_to_integer({.pos = position, .size = size});
        const auto fullscreen              = state.get_option<bool>("fullscreen", false);
        bool       window_settings_changed = window_rect_.observe(rect);
        window_settings_changed |= fullscreen_.observe(fullscreen);
        window_settings_changed |= monitor_id_.observe(monitor_id);

        const bool presenter_settings_changed = presenter_settings_.observe(presenter_settings);
        if (presenter_ && (presenter_settings_changed || window_settings_changed) && !presenter_stopping_) {
            presenter_->request_stop();
            presenter_stopping_ = true;
        }

        if (presenter_stopping_) {
            if (!presenter_->stopped()) {
                app->status_registry()->write(id_, status::connected_status_s{.connected = false});
                return;
            }
            destroy_presenter();
        }

        if (!enabled) {
            app->status_registry()->write(id_, status::connected_status_s{.connected = false});
            return;
        }

        result->demands_execution = true;

        if (!presenter_) {
            presenter_ = std::make_unique<output_presenter_s>(
                app->ctx(),
                static_cast<size_t>(app->frame_settings().screen_output.buffer_frames),
                nominal_frame_duration,
                fullscreen,
                monitor_id,
                rect);
            presenter_->start();
        }

        app->status_registry()->write(id_, status::connected_status_s{.connected = true});
        publish_metrics(app->status_registry());
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (!presenter_) {
            return;
        }

        auto* texture    = iface_tex_.resolve_value(app, nodes, state);
        auto  dimensions = texture != nullptr ? texture->texture_dimensions() : gpu::vec2i_t{128, 128};
        auto  frame      = presenter_->try_acquire(dimensions);
        if (!frame.has_value()) {
            return;
        }

        auto* target = frame->target();
        target->begin_render(gpu::framebuffer_s::load_op_e::clear);
        if (texture != nullptr) {
            if (!textured_quad_) {
                auto* shader   = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
                textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader, gpu::textured_quad_s::uv_e::regular);
            }
            textured_quad_->draw(texture);
        }
        gpu::framebuffer_s::end_render();
        const auto fill_mode          = state.get_enum_option_unchecked<gpu::fill_mode_e>("fill_mode");
        const auto content_dimensions = texture != nullptr ? texture->display_dimensions() : dimensions;
        frame->submit(app->frame_context().program_target_time, fill_mode, content_dimensions);
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Screen output"                        },
            {"enabled",    true                                   },
            {"fullscreen", false                                  },
            {"fill_mode",  enum_to_string(gpu::fill_mode_e::scale)},
            {"monitor_id", ""                                     },
            {"position",   gpu::vec2_t{0, 0}                      },
            {"size",       gpu::vec2_t{100, 100}                  },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "enabled" || name == "fullscreen") {
            return normalize_option_value<bool>(value);
        }

        if (name == "fill_mode") {
            return normalize_enum_option_value<gpu::fill_mode_e>(value);
        }

        if (name == "monitor_id") {
            return normalize_option_value<std::string_view>(value);
        }

        if (name == "position") {
            auto result = normalize_option_value<gpu::vec2_t>(value);
            if (result == option_result_e::invalid) {
                return result;
            }

            const auto normalized = value->get<gpu::vec2_t>();
            const auto rounded    = gpu::vec2_t(gpu::round_to_integer(normalized));
            *value                = rounded;
            return rounded == normalized ? result : option_result_e::corrected;
        }

        if (name == "size") {
            auto result = normalize_option_value<gpu::vec2_t>(value, gpu::vec2_t{100, 100});
            if (result == option_result_e::invalid) {
                return result;
            }

            const auto normalized = value->get<gpu::vec2_t>();
            const auto rounded    = gpu::vec2_t(gpu::round_to_integer(normalized));
            *value                = rounded;
            return rounded == normalized ? result : option_result_e::corrected;
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "screen_output"; }
};

} // namespace

namespace miximus::nodes::screen {

std::shared_ptr<node_i> create_screen_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::screen
