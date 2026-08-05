#pragma once
#include "frame_rate.hpp"
#include "settings_option.hpp"
#include "utils/flicks.hpp"

#include <boost/describe.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace miximus::status {

struct connected_status_s
{
    bool connected{};
};

struct device_names_status_s
{
    std::vector<settings_option_s> device_names;
};

struct display_modes_status_s
{
    std::vector<settings_option_s> display_modes;
};

struct source_names_status_s
{
    std::vector<settings_option_s> source_names;
};

struct monitor_options_status_s
{
    std::vector<settings_option_s> monitors;
};

struct font_names_status_s
{
    std::vector<settings_option_s> font_names;
};

struct font_variants_status_s
{
    std::vector<settings_option_s> font_variants;
};

struct application_frame_status_s
{
    frame_rate_s frame_rate;
    int64_t      frame_duration_flicks{};
    uint64_t     epoch{};
};

struct application_lifecycle_status_s
{
    int64_t prepare_duration_us{};
    int64_t submit_duration_us{};
    int64_t execute_duration_us{};
    int64_t gpu_finish_duration_us{};
    int64_t complete_duration_us{};
    size_t  demanding_node_count{};
    size_t  submitted_node_count{};
    size_t  executed_node_count{};
};

struct application_scheduler_status_s
{
    std::string clock_source;
    uint64_t    frame_number{};
    int64_t     pts_flicks{};
    int64_t     render_duration_us{};
    int64_t     render_duration_max_us{};
    uint64_t    render_duration_max_frame{};
    int64_t     start_lateness_us{};
    int64_t     start_lateness_max_us{};
    uint64_t    start_lateness_max_frame{};
    int64_t     deadline_margin_us{};
    int64_t     deadline_margin_min_us{};
    uint64_t    deadline_margin_min_frame{};
    uint64_t    deadline_misses_total{};
    uint64_t    skipped_frames_last{};
    uint64_t    skipped_frames_total{};
    bool        sustained_overload{};
};

struct render_delay_test_status_s
{
    int64_t  test_render_delay_ms{};
    uint64_t test_render_delay_every{};
    uint64_t test_render_delay_injections{};
};

struct source_timing_status_s
{
    uint64_t                     source_queue_pushed{};
    size_t                       source_queue_depth{};
    uint64_t                     source_queue_overflow_drops{};
    uint64_t                     source_queue_selection_drops{};
    uint64_t                     source_queue_repeated{};
    uint64_t                     source_queue_starvation_repeats{};
    uint64_t                     source_queue_timing_repeats{};
    uint64_t                     source_queue_missing{};
    uint64_t                     source_queue_discontinuities{};
    uint64_t                     source_queue_transfer_failures{};
    uint64_t                     source_queue_transfer_cancellations{};
    std::optional<double>        source_recovered_rate;
    std::optional<double>        source_observed_rate;
    std::optional<utils::flicks> source_phase_offset_us;
    std::optional<utils::flicks> source_phase_error_us;
    std::optional<utils::flicks> source_phase_adjustment_us;
    std::optional<utils::flicks> source_repeat_next_frame_lead_min_us;
    std::optional<utils::flicks> source_repeat_next_frame_lead_max_us;
};

struct decklink_input_device_status_s
{
    std::optional<bool>        signal_locked;
    std::optional<bool>        ancillary_signal_locked;
    std::optional<bool>        capture_busy;
    std::optional<int64_t>     pcie_link_width;
    std::optional<int64_t>     pcie_link_speed;
    std::optional<int64_t>     temperature_c;
    std::optional<std::string> active_format;
    std::optional<std::string> detected_format;
    std::optional<std::string> detected_colorspace;
    std::optional<std::string> detected_dynamic_range;
    std::optional<std::string> detected_field_dominance;
    std::optional<std::string> detected_sdi_link_configuration;
    std::optional<std::string> input_pixel_format;
};

struct decklink_output_device_status_s
{
    std::optional<bool>        reference_locked;
    std::optional<bool>        playback_busy;
    std::optional<int64_t>     pcie_link_width;
    std::optional<int64_t>     pcie_link_speed;
    std::optional<int64_t>     temperature_c;
    std::optional<std::string> active_format;
    std::optional<std::string> output_pixel_format;
    std::optional<std::string> reference_format;
};

struct decklink_input_metrics_status_s
{
    uint64_t frames_received{};
    uint64_t frames_missing{};
    uint64_t no_input_source_frames{};
    uint64_t upload_slot_drops{};
    uint64_t upload_acquire_slow_count{};
    uint64_t upload_acquire_failures{};
    uint64_t upload_acquire_wait_max_us{};
    uint64_t content_frames_sampled{};
    uint64_t content_frame_repeats{};
    uint64_t content_repeat_streak{};
    uint64_t content_repeat_streak_max{};
    size_t   available_video_frames{};
};

struct ndi_input_metrics_status_s
{
    uint64_t frames_received{};
    uint64_t invalid_frames{};
    uint64_t receiver_video_drops{};
    size_t   receiver_queue_depth{};
    uint64_t upload_slot_drops{};
};

struct download_stream_status_s
{
    size_t   download_slots{};
    size_t   download_slots_free{};
    size_t   download_slots_rendering{};
    size_t   download_slots_queued{};
    size_t   download_slots_ready{};
    size_t   download_slots_cpu_reading{};
    size_t   download_pending_allocations{};
    uint64_t download_acquire_misses{};
    uint64_t download_transfers_completed{};
    uint64_t download_transfer_failures{};
    int64_t  download_transfer_duration_total_us{};
    int64_t  download_transfer_duration_max_us{};
    bool     download_allocation_failed{};
};

struct ndi_output_metrics_status_s
{
    uint64_t program_frames_received{};
    uint64_t program_queue_overflow_drops{};
    uint64_t program_timing_drops{};
    uint64_t program_frames_repeated{};
    uint64_t program_frames_missing{};
    uint64_t output_intervals_skipped{};
    uint64_t frames_sent{};
    size_t   queued_frames{};
    int64_t  output_latency_us{};
    int64_t  program_selection_offset_us{};
    uint64_t render_target_drops{};
};

struct decklink_output_metrics_status_s
{
    uint64_t frames_completed{};
    uint64_t frames_displayed_late{};
    uint64_t frames_dropped{};
    uint64_t frames_flushed{};
    uint64_t program_frames_received{};
    uint64_t program_queue_overflow_drops{};
    uint64_t program_timing_drops{};
    uint64_t program_frames_repeated{};
    uint64_t program_frames_missing{};
    uint64_t program_cadence_repeats{};
    uint64_t program_starvation_repeats{};
    uint64_t program_starvation_repeat_streak{};
    uint64_t program_starvation_repeat_streak_max{};
    uint64_t output_refill_shortfalls{};
    uint64_t content_frames_sampled{};
    uint64_t content_frame_repeats{};
    uint64_t content_repeat_streak{};
    uint64_t content_repeat_streak_max{};
    uint64_t completion_intervals{};
    int64_t  completion_interval_max_us{};
    size_t   program_queue_depth{};
    size_t   program_queue_depth_max{};
    uint64_t buffered_video_frames{};
    uint64_t buffered_video_frames_min{};
    uint64_t buffered_video_frames_max{};
    uint64_t buffered_below_target_samples{};
    uint64_t buffered_zero_samples{};
    int64_t  output_latency_us{};
    int64_t  program_selection_offset_us{};
    uint64_t completion_time_failures{};
    uint64_t render_target_drops{};
};

struct screen_output_metrics_status_s
{
    std::string clock_quality;
    uint64_t    frames_submitted{};
    uint64_t    program_queue_overflow_drops{};
    uint64_t    program_timing_drops{};
    uint64_t    program_frames_repeated{};
    uint64_t    program_frames_missing{};
    uint64_t    output_intervals_skipped{};
    uint64_t    swaps_completed{};
    uint64_t    render_acquire_misses{};
    size_t      queued_frames{};
    size_t      render_slots{};
    size_t      render_slots_free{};
    size_t      render_slots_retiring{};
    int64_t     output_latency_us{};
    int64_t     program_selection_offset_us{};
    int64_t     completion_interval_max_us{};
    double      measured_refresh_hz{};
};

BOOST_DESCRIBE_STRUCT(connected_status_s, (), (connected))
BOOST_DESCRIBE_STRUCT(device_names_status_s, (), (device_names))
BOOST_DESCRIBE_STRUCT(display_modes_status_s, (), (display_modes))
BOOST_DESCRIBE_STRUCT(source_names_status_s, (), (source_names))
BOOST_DESCRIBE_STRUCT(monitor_options_status_s, (), (monitors))
BOOST_DESCRIBE_STRUCT(font_names_status_s, (), (font_names))
BOOST_DESCRIBE_STRUCT(font_variants_status_s, (), (font_variants))
BOOST_DESCRIBE_STRUCT(application_frame_status_s, (), (frame_rate, frame_duration_flicks, epoch))
BOOST_DESCRIBE_STRUCT(application_lifecycle_status_s,
                      (),
                      (prepare_duration_us,
                       submit_duration_us,
                       execute_duration_us,
                       gpu_finish_duration_us,
                       complete_duration_us,
                       demanding_node_count,
                       submitted_node_count,
                       executed_node_count))
BOOST_DESCRIBE_STRUCT(application_scheduler_status_s,
                      (),
                      (clock_source,
                       frame_number,
                       pts_flicks,
                       render_duration_us,
                       render_duration_max_us,
                       render_duration_max_frame,
                       start_lateness_us,
                       start_lateness_max_us,
                       start_lateness_max_frame,
                       deadline_margin_us,
                       deadline_margin_min_us,
                       deadline_margin_min_frame,
                       deadline_misses_total,
                       skipped_frames_last,
                       skipped_frames_total,
                       sustained_overload))
BOOST_DESCRIBE_STRUCT(render_delay_test_status_s,
                      (),
                      (test_render_delay_ms, test_render_delay_every, test_render_delay_injections))
BOOST_DESCRIBE_STRUCT(source_timing_status_s,
                      (),
                      (source_queue_pushed,
                       source_queue_depth,
                       source_queue_overflow_drops,
                       source_queue_selection_drops,
                       source_queue_repeated,
                       source_queue_starvation_repeats,
                       source_queue_timing_repeats,
                       source_queue_missing,
                       source_queue_discontinuities,
                       source_queue_transfer_failures,
                       source_queue_transfer_cancellations,
                       source_recovered_rate,
                       source_observed_rate,
                       source_phase_offset_us,
                       source_phase_error_us,
                       source_phase_adjustment_us,
                       source_repeat_next_frame_lead_min_us,
                       source_repeat_next_frame_lead_max_us))
BOOST_DESCRIBE_STRUCT(decklink_input_device_status_s,
                      (),
                      (signal_locked,
                       ancillary_signal_locked,
                       capture_busy,
                       pcie_link_width,
                       pcie_link_speed,
                       temperature_c,
                       active_format,
                       detected_format,
                       detected_colorspace,
                       detected_dynamic_range,
                       detected_field_dominance,
                       detected_sdi_link_configuration,
                       input_pixel_format))
BOOST_DESCRIBE_STRUCT(decklink_output_device_status_s,
                      (),
                      (reference_locked,
                       playback_busy,
                       pcie_link_width,
                       pcie_link_speed,
                       temperature_c,
                       active_format,
                       output_pixel_format,
                       reference_format))
BOOST_DESCRIBE_STRUCT(decklink_input_metrics_status_s,
                      (),
                      (frames_received,
                       frames_missing,
                       no_input_source_frames,
                       upload_slot_drops,
                       upload_acquire_slow_count,
                       upload_acquire_failures,
                       upload_acquire_wait_max_us,
                       content_frames_sampled,
                       content_frame_repeats,
                       content_repeat_streak,
                       content_repeat_streak_max,
                       available_video_frames))
BOOST_DESCRIBE_STRUCT(ndi_input_metrics_status_s,
                      (),
                      (frames_received, invalid_frames, receiver_video_drops, receiver_queue_depth, upload_slot_drops))
BOOST_DESCRIBE_STRUCT(download_stream_status_s,
                      (),
                      (download_slots,
                       download_slots_free,
                       download_slots_rendering,
                       download_slots_queued,
                       download_slots_ready,
                       download_slots_cpu_reading,
                       download_pending_allocations,
                       download_acquire_misses,
                       download_transfers_completed,
                       download_transfer_failures,
                       download_transfer_duration_total_us,
                       download_transfer_duration_max_us,
                       download_allocation_failed))
BOOST_DESCRIBE_STRUCT(ndi_output_metrics_status_s,
                      (),
                      (program_frames_received,
                       program_queue_overflow_drops,
                       program_timing_drops,
                       program_frames_repeated,
                       program_frames_missing,
                       output_intervals_skipped,
                       frames_sent,
                       queued_frames,
                       output_latency_us,
                       program_selection_offset_us,
                       render_target_drops))
BOOST_DESCRIBE_STRUCT(decklink_output_metrics_status_s,
                      (),
                      (frames_completed,
                       frames_displayed_late,
                       frames_dropped,
                       frames_flushed,
                       program_frames_received,
                       program_queue_overflow_drops,
                       program_timing_drops,
                       program_frames_repeated,
                       program_frames_missing,
                       program_cadence_repeats,
                       program_starvation_repeats,
                       program_starvation_repeat_streak,
                       program_starvation_repeat_streak_max,
                       output_refill_shortfalls,
                       content_frames_sampled,
                       content_frame_repeats,
                       content_repeat_streak,
                       content_repeat_streak_max,
                       completion_intervals,
                       completion_interval_max_us,
                       program_queue_depth,
                       program_queue_depth_max,
                       buffered_video_frames,
                       buffered_video_frames_min,
                       buffered_video_frames_max,
                       buffered_below_target_samples,
                       buffered_zero_samples,
                       output_latency_us,
                       program_selection_offset_us,
                       completion_time_failures,
                       render_target_drops))
BOOST_DESCRIBE_STRUCT(screen_output_metrics_status_s,
                      (),
                      (clock_quality,
                       frames_submitted,
                       program_queue_overflow_drops,
                       program_timing_drops,
                       program_frames_repeated,
                       program_frames_missing,
                       output_intervals_skipped,
                       swaps_completed,
                       render_acquire_misses,
                       queued_frames,
                       render_slots,
                       render_slots_free,
                       render_slots_retiring,
                       output_latency_us,
                       program_selection_offset_us,
                       completion_interval_max_us,
                       measured_refresh_hz))

} // namespace miximus::status
