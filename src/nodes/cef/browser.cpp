#include "browser_inputs.hpp"
#include "browser_options.hpp"
#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "nodes/action.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "subsystem.hpp"
#include "types/node_status_json.hpp"

#include <bit>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

class node_impl final : public node_i
{
    output_interface_s<const gpu::texture_s*> iface_tex_{*this, "tex"};
    cef::browser_inputs_s                     inputs_{*this};
    uint32_t                                  input_mask_{};
    using session_t = cef::session_s;
    std::unique_ptr<cef::session_request_s> request_;
    std::shared_ptr<session_t>              session_;
    session_t::frame_ptr_t                  output_;
    std::optional<session_t::options_s>     selection_;
    std::chrono::steady_clock::time_point   retry_after_;
    std::chrono::steady_clock::time_point   next_metrics_;
    uint64_t                                restarts_{};
    std::string                             error_;
    status::cef_browser_status_s            browser_status_;

    static session_t::options_s session_options(core::app_state_s* app, const node_state_s& state)
    {
        const auto url  = state.get_option<std::string>("url");
        const auto rate = app->frame_settings().frame_rate;
        const auto size = state.get_option<gpu::vec2_t>("size");
        return {
            .url        = url,
            .dimensions = {static_cast<int>(size.x), static_cast<int>(size.y)},
            // CEF's public cadence is integer. The existing timed queue handles
            // fractional program rates; capture never skips received paints.
            .frame_rate = static_cast<int>((rate.numerator + uint64_t(rate.denominator) - 1) / rate.denominator),
        };
    }

    void stop()
    {
        iface_tex_.set_value(nullptr);
        output_.reset();
        session_.reset();
        request_.reset();
        browser_status_ = {};
        input_mask_     = 0;
    }

    void fail(std::string error)
    {
        error_ = std::move(error);
        stop();
        retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1ULL << restarts_);
    }

    void publish_metrics(core::node_status_registry_s*              status,
                         const std::optional<session_t::metrics_s>& metrics,
                         std::chrono::steady_clock::time_point      now)
    {
        auto& browser_status = browser_status_;
        if (metrics) {
            browser_status.cef_state = metrics->phase;
            browser_status.cef_error = metrics->error;
        } else {
            browser_status.cef_state = request_ ? cef_state_e::starting : cef_state_e::failed;
            browser_status.cef_error = error_;
        }
        browser_status.cef_restarts = restarts_;
        // Publish lifecycle changes every frame while retaining the last counter snapshot.
        if (now < next_metrics_ && metrics) {
            status->write(id_, browser_status);
            return;
        }
        if (metrics) {
            const auto& inputs = metrics->inputs;
            if (!inputs.available) {
                browser_status.cef_inputs_state = cef_input_state_e::unavailable;
            } else if (inputs.failed) {
                browser_status.cef_inputs_state = cef_input_state_e::failed;
            } else {
                browser_status.cef_inputs_state =
                    inputs.subscribed != 0 ? cef_input_state_e::active : cef_input_state_e::idle;
            }

            browser_status.cef_inputs_error                 = inputs.error;
            browser_status.cef_inputs_active                = std::popcount(inputs.subscribed);
            browser_status.cef_inputs_submitted             = inputs.submitted;
            browser_status.cef_inputs_delivered             = inputs.delivered;
            browser_status.cef_inputs_drops                 = inputs.drops;
            browser_status.cef_inputs_held                  = inputs.occupied;
            browser_status.cef_inputs_reserved_bytes        = inputs.reserved_bytes;
            browser_status.cef_inputs_export_bytes          = inputs.export_bytes;
            browser_status.cef_paints                       = metrics->received;
            browser_status.cef_copies                       = metrics->copied;
            browser_status.cef_capacity_drops               = metrics->dropped;
            browser_status.cef_timing_rejections            = metrics->timing_rejections;
            browser_status.cef_timing_error                 = metrics->timing_error;
            browser_status.cef_capture_p50_upper_us         = metrics->capture.p50_upper_us;
            browser_status.cef_capture_p95_upper_us         = metrics->capture.p95_upper_us;
            browser_status.cef_capture_p99_upper_us         = metrics->capture.p99_upper_us;
            browser_status.cef_capture_max_us               = metrics->capture.maximum_us;
            browser_status.cef_completion_wait_p95_upper_us = metrics->completion_wait.p95_upper_us;
            browser_status.cef_completion_wait_max_us       = metrics->completion_wait.maximum_us;
        }
        status->write(id_, browser_status);
        if (metrics) {
            const auto& queue = metrics->source_queue;
            status->write(id_,
                          status::source_timing_status_s{
                              .source_queue_pushed                  = queue.pushed,
                              .source_queue_depth                   = queue.queued,
                              .source_queue_overflow_drops          = queue.overflow_drops,
                              .source_queue_selection_drops         = queue.selection_drops,
                              .source_queue_repeated                = queue.repeated,
                              .source_queue_starvation_repeats      = queue.starvation_repeats,
                              .source_queue_timing_repeats          = queue.timing_repeats,
                              .source_queue_missing                 = queue.missing,
                              .source_queue_discontinuities         = queue.discontinuities,
                              .source_queue_transfer_failures       = queue.transfer_failures,
                              .source_queue_transfer_cancellations  = queue.transfer_cancellations,
                              .source_recovered_rate                = queue.recovered_rate,
                              .source_observed_rate                 = queue.observed_rate,
                              .source_phase_offset_us               = queue.phase_offset,
                              .source_phase_error_us                = queue.phase_error,
                              .source_phase_adjustment_us           = queue.phase_adjustment,
                              .source_repeat_next_frame_lead_min_us = queue.repeat_next_frame_lead_min,
                              .source_repeat_next_frame_lead_max_us = queue.repeat_next_frame_lead_max,
                          });
        }
        next_metrics_ = now + 1s;
    }

    void update_session(cef::subsystem_s&                     subsystem,
                        const session_t::options_s&           selection,
                        std::chrono::steady_clock::time_point now)
    {
        if (!request_ && now >= retry_after_ && (error_.empty() || restarts_ < 3)) {
            if (!error_.empty()) {
                ++restarts_;
            }
            request_ = subsystem.create_session(selection);
        }
        if (request_ && !session_) {
            session_ = request_->session();
            if (auto error = request_->error(); !error.empty()) {
                fail(std::move(error));
            }
        }
    }

  public:
    node_impl()                                  = default;
    node_impl(const node_impl& other)            = delete;
    node_impl& operator=(const node_impl& other) = delete;
    node_impl(node_impl&& other)                 = delete;
    node_impl& operator=(node_impl&& other)      = delete;

    ~node_impl() override { stop(); }

    action_result_s handle_action(core::app_state_s*    app,
                                  const node_state_s&   state,
                                  std::string_view      name,
                                  const nlohmann::json& payload) final
    {
        if (name != "reload") {
            return {.error = error_e::unsupported_action, .message = "Unknown browser action"};
        }
        if (!payload.is_object() || payload.size() > 1 ||
            (payload.size() == 1 && (!payload.contains("ignore_cache") || !payload.at("ignore_cache").is_boolean()))) {
            return {.error   = error_e::invalid_payload,
                    .message = "Reload expects an object with optional boolean ignore_cache"};
        }
        if (!state.get_option<bool>("enabled") || state.get_option<std::string>("url").empty() || !request_ ||
            !session_ || !selection_ || *selection_ != session_options(app, state)) {
            return {.error = error_e::unavailable, .message = "Browser has no active session to reload"};
        }
        if (!request_->reload(payload.value("ignore_cache", false))) {
            return {.error = error_e::busy, .message = "Browser is not ready to reload"};
        }
        next_metrics_ = {};
        return {};
    }

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* result) final
    {
        auto*      status  = app->status_registry();
        const bool enabled = state.get_option<bool>("enabled");
        const auto url     = state.get_option<std::string>("url");
        if (app->cef_subsystem() == nullptr || !enabled || url.empty()) {
            stop();
            selection_.reset();
            status->write(id_, status::connected_status_s{.connected = false});
            status->write(id_,
                          status::cef_browser_status_s{
                              .cef_state = !enabled || url.empty() ? cef_state_e::stopped : cef_state_e::unavailable,
                              .cef_error = !enabled || url.empty() ? "" : app->cef_error(),
                              .cef_inputs_error = {},
                          });
            return;
        }
        const auto selection = session_options(app, state);
        if (selection_ != selection) {
            stop();
            selection_ = selection;
            error_.clear();
            restarts_     = 0;
            retry_after_  = {};
            next_metrics_ = {};
        }
        const auto now = std::chrono::steady_clock::now();
        update_session(*app->cef_subsystem(), selection, now);
        std::optional<session_t::metrics_s> metrics;
        if (session_) {
            const auto& frame = app->frame_context();
            session_->advance_frames(frame.program_pts, frame.program_target_time, frame.discontinuity);
            session_->send_program_time(frame);
            metrics = session_->metrics();
            if (metrics->phase == session_t::phase_e::failed || metrics->phase == session_t::phase_e::closed) {
                fail(metrics->error.empty() ? "Browser closed unexpectedly" : metrics->error);
                metrics.reset();
            } else if (metrics->inputs.failed) {
                // Recover renderer and retirement failures through the bounded
                // session restart policy. Only unproven GPU retirement quarantines
                // allocations; ordinary copy failures still retire safely.
                fail(metrics->inputs.error.empty() ? "Browser input transport failed" : metrics->inputs.error);
                browser_status_.cef_inputs_state = cef_input_state_e::failed;
                browser_status_.cef_inputs_error = error_;
                metrics.reset();
            }
        }
        status->write(id_,
                      status::connected_status_s{
                          .connected = metrics && metrics->phase == session_t::phase_e::ready,
                      });
        input_mask_               = session_ ? session_->media_input_demand() : 0;
        result->demands_execution = input_mask_ != 0;
        publish_metrics(status, metrics, now);
    }

    void submit(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (session_) {
            (void)session_->submit_frame(app->frame_context().program_pts);
            for (size_t input = 0; input < inputs_.ports.size(); ++input) {
                if ((input_mask_ & (1U << input)) != 0U) {
                    interface_i::submit_dependencies(app, nodes, inputs_.ports.at(input).connections(state));
                }
            }
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        output_ = session_ ? session_->resolve_frame() : nullptr;
        iface_tex_.set_value(output_ ? &output_->texture() : nullptr);
        if (session_) {
            for (size_t input = 0; input < inputs_.ports.size(); ++input) {
                if ((input_mask_ & (1U << input)) == 0U) {
                    continue;
                }

                const auto*            source      = inputs_.ports.at(input).resolve_value(app, nodes, state);
                const auto             connections = inputs_.ports.at(input).connections(state);
                const std::string_view source_node =
                    connections.empty() ? std::string_view{} : connections.front().from_node;
                const std::string_view source_interface =
                    connections.empty() ? std::string_view{} : connections.front().from_interface;
                if (auto publish = session_->record_media_input(
                        input,
                        app->commands(),
                        source,
                        source_node,
                        source_interface,
                        std::chrono::duration_cast<std::chrono::microseconds>(app->frame_context().program_pts)
                            .count())) {
                    app->defer_output([publish = std::move(publish)](const gpu::completion_s&) { publish(); });
                }
            }
        }
    }

    void complete(core::app_state_s* /* app */) final
    {
        iface_tex_.set_value(nullptr);
        output_.reset();
        if (session_) {
            session_->release_prepared_frame();
        }
    }

    nlohmann::json  get_default_options() const final { return cef::browser_default_options(); }
    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        return cef::normalize_browser_option(name, value);
    }
    std::string_view type() const final { return "cef_browser"; }
};
} // namespace

namespace miximus::nodes::cef {
std::shared_ptr<node_i> create_browser_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::cef
