#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "types/node_status_json.hpp"
#if MIXIMUS_ENABLE_CEF
#include "subsystem.hpp"
#endif

#include <chrono>
#include <cmath>
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
#if MIXIMUS_ENABLE_CEF
    using session_t = cef::detail::browser_session_s;
    std::unique_ptr<cef::session_request_s> request_;
    std::shared_ptr<session_t>              session_;
    session_t::frame_ptr_t                  output_;
    std::optional<session_t::options_s>     selection_;
    std::chrono::steady_clock::time_point   retry_after_{};
    std::chrono::steady_clock::time_point   next_metrics_{};
    uint64_t                                restarts_{};
    std::string                             error_;

    void stop()
    {
        iface_tex_.set_value(nullptr);
        output_.reset();
        if (session_)
            session_->reset_frames();
        session_.reset();
        request_.reset();
    }

    void fail(std::string error)
    {
        error_ = std::move(error);
        stop();
        retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1ULL << restarts_);
    }

    static std::string_view phase_name(session_t::phase_e phase)
    {
        switch (phase) {
            case session_t::phase_e::starting:
                return "starting";
            case session_t::phase_e::loading:
                return "loading";
            case session_t::phase_e::ready:
                return "ready";
            case session_t::phase_e::closing:
                return "closing";
            case session_t::phase_e::closed:
                return "closed";
            case session_t::phase_e::failed:
                return "failed";
        }
        return "failed";
    }
#endif

  public:
    ~node_impl() override
    {
#if MIXIMUS_ENABLE_CEF
        stop();
#endif
    }

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s*) final
    {
        auto*      status  = app->status_registry();
        const bool enabled = state.get_option<bool>("enabled");
        const auto url     = state.get_option<std::string>("url");
        if (!app->cef_subsystem() || !enabled || url.empty()) {
#if MIXIMUS_ENABLE_CEF
            stop();
            selection_.reset();
#endif
            status->write(id_, status::connected_status_s{.connected = false});
            status->write(id_,
                          status::cef_browser_status_s{
                              .cef_state = !enabled || url.empty() ? "stopped" : "unavailable",
                              .cef_error = !enabled || url.empty() ? "" : app->cef_error(),
                          });
            return;
        }
#if MIXIMUS_ENABLE_CEF
        const auto                 rate = app->frame_settings().frame_rate;
        const auto                 size = state.get_option<gpu::vec2_t>("size");
        const session_t::options_s selection{
            .url        = url,
            .dimensions = {static_cast<int>(size.x), static_cast<int>(size.y)},
            // CEF's public cadence is integer. The existing timed queue handles
            // fractional program rates; capture never skips received paints.
            .frame_rate = static_cast<int>((rate.numerator + uint64_t(rate.denominator) - 1) / rate.denominator),
        };
        if (selection_ != selection) {
            stop();
            selection_ = selection;
            error_.clear();
            restarts_     = 0;
            retry_after_  = {};
            next_metrics_ = {};
        }
        const auto now = std::chrono::steady_clock::now();
        if (!request_ && now >= retry_after_ && (error_.empty() || restarts_ < 3)) {
            if (!error_.empty())
                ++restarts_;
            request_ = app->cef_subsystem()->create_session(selection);
        }
        if (request_ && !session_) {
            session_ = request_->session();
            if (auto error = request_->error(); !error.empty())
                fail(std::move(error));
        }
        std::optional<session_t::metrics_s> metrics;
        if (session_) {
            const auto& frame = app->frame_context();
            session_->advance_frames(frame.program_pts, frame.program_target_time, frame.discontinuity);
            metrics = session_->metrics();
            if (metrics->phase == session_t::phase_e::failed || metrics->phase == session_t::phase_e::closed) {
                fail(metrics->error.empty() ? "Browser closed unexpectedly" : metrics->error);
                metrics.reset();
            }
        }
        status->write(id_,
                      status::connected_status_s{
                          .connected = metrics && metrics->phase == session_t::phase_e::ready,
                      });
        // Lifecycle/error deltas are immediate; high-frequency counters are not.
        if (now >= next_metrics_ || !metrics) {
            status->write(id_,
                          status::cef_browser_status_s{
                              .cef_state          = metrics    ? std::string(phase_name(metrics->phase))
                                                    : request_ ? "starting"
                                                               : "failed",
                              .cef_error          = metrics ? metrics->error : error_,
                              .cef_paints         = metrics ? metrics->received : 0,
                              .cef_copies         = metrics ? metrics->copied : 0,
                              .cef_capacity_drops = metrics ? metrics->dropped : 0,
                              .cef_restarts       = restarts_,
                          });
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
#endif
    }

    void submit(core::app_state_s* app, const node_map_t&, const node_state_s&) final
    {
#if MIXIMUS_ENABLE_CEF
        if (session_)
            (void)session_->submit_frame(app->frame_context().program_pts);
#endif
    }

    void execute(core::app_state_s*, const node_map_t&, const node_state_s&) final
    {
#if MIXIMUS_ENABLE_CEF
        output_ = session_ ? session_->resolve_frame() : nullptr;
        iface_tex_.set_value(output_ ? &output_->texture() : nullptr);
#else
        iface_tex_.set_value(nullptr);
#endif
    }

    void complete(core::app_state_s*) final
    {
#if MIXIMUS_ENABLE_CEF
        iface_tex_.set_value(nullptr);
        output_.reset();
        if (session_)
            session_->release_prepared_frame();
#endif
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",    "Browser"              },
            {"enabled", true                   },
            {"url",     "about:blank"          },
            {"size",    gpu::vec2_t{1920, 1080}},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "enabled")
            return normalize_option_value<bool>(value);
        if (name == "size") {
            auto result = normalize_option_value<gpu::vec2_t>(value, gpu::vec2_t{1, 1}, gpu::vec2_t{4096, 4096});
            if (result == option_result_e::invalid)
                return result;
            for (auto& component : *value) {
                const auto rounded = std::round(component.get<double>());
                if (component != rounded) {
                    component = rounded;
                    result    = option_result_e::corrected;
                }
            }
            return result;
        }
        if (name == "url") {
            const auto result = normalize_option_value<std::string_view>(value);
            if (result == option_result_e::invalid || value->get_ref<const std::string&>().size() > 65536)
                return option_result_e::invalid;
            return result;
        }
        return option_result_e::invalid;
    }
    std::string_view type() const final { return "cef_browser"; }
};
} // namespace

namespace miximus::nodes::cef {
std::shared_ptr<node_i> create_browser_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::cef
