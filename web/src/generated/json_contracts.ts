// Generated from the described C++ JSON contracts. Do not edit manually.

export const enum action_e {
  subscribe = "subscribe",
  unsubscribe = "unsubscribe",
  ping = "ping",
  socket_info = "socket_info",
  command = "command",
  result = "result",
  error = "error",
}

export const enum topic_e {
  add_node = "add_node",
  remove_node = "remove_node",
  add_connection = "add_connection",
  remove_connection = "remove_connection",
  update_node = "update_node",
  font_registry = "font_registry",
  config = "config",
  node_status = "node_status",
}

export const enum error_e {
  no_error = "no_error",
  internal_error = "internal_error",
  malformed_payload = "malformed_payload",
  invalid_topic = "invalid_topic",
  invalid_type = "invalid_type",
  duplicate_id = "duplicate_id",
  invalid_options = "invalid_options",
  not_found = "not_found",
  circular_connection = "circular_connection",
}

export const enum font_registry_command_e {
  refresh = "refresh",
}

export interface settings_option_s {
  readonly id: string;
  readonly label: string;
}

export interface frame_rate_s {
  readonly numerator: number;
  readonly denominator: number;
}

export type vec2_t = [number, number];

export interface options_s {
  readonly node_visual_position?: vec2_t;
  readonly name?: string;
  readonly [key: string]: unknown;
}

export interface message_s {
  readonly action: action_e;
  readonly token?: string | null;
}

export interface command_s {
  readonly action: action_e;
  readonly token?: string | null;
  readonly topic: topic_e;
  readonly origin_id?: number | null;
}

export interface connection_s {
  readonly from_node: string;
  readonly from_interface: string;
  readonly to_node: string;
  readonly to_interface: string;
}

export interface node_s {
  readonly type: string;
  readonly id: string;
  readonly schema_version?: number | null;
  readonly options: options_s;
}

export interface config_s {
  readonly schema_version: number;
  readonly nodes: readonly node_s[];
  readonly connections: readonly connection_s[];
  readonly status?: Readonly<Record<string, node_status_s>> | null;
}

export interface subscribe_request_s {
  readonly action: action_e.subscribe;
  readonly token?: string | null;
  readonly topic: topic_e;
}

export interface unsubscribe_request_s {
  readonly action: action_e.unsubscribe;
  readonly token?: string | null;
  readonly topic: topic_e;
}

export interface add_node_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.add_node;
  readonly token?: string | null;
  readonly node: node_s;
}

export interface remove_node_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.remove_node;
  readonly token?: string | null;
  readonly id: string;
}

export interface update_node_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.update_node;
  readonly token?: string | null;
  readonly id: string;
  readonly options: options_s;
}

export interface add_connection_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.add_connection;
  readonly token?: string | null;
  readonly connection: connection_s;
}

export interface remove_connection_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.remove_connection;
  readonly token?: string | null;
  readonly connection: connection_s;
}

export interface font_registry_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.font_registry;
  readonly token?: string | null;
  readonly command: font_registry_command_e;
}

export interface config_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.config;
  readonly token?: string | null;
}

export interface node_status_request_s {
  readonly action: action_e.command;
  readonly topic: topic_e.node_status;
  readonly token?: string | null;
  readonly id: string;
}

export interface ping_response_s {
  readonly action: action_e.ping;
  readonly response: boolean;
}

export interface socket_info_s {
  readonly action: action_e.socket_info;
  readonly id: number;
  readonly bundle_hash: string;
}

export interface result_s {
  readonly action: action_e.result;
  readonly token: string;
}

export interface config_result_s {
  readonly action: action_e.result;
  readonly token: string;
  readonly config: config_s;
}

export interface node_status_result_s {
  readonly action: action_e.result;
  readonly token: string;
  readonly id: string;
  readonly status: node_status_s;
}

export interface error_s {
  readonly action: action_e.error;
  readonly token: string;
  readonly error: error_e;
  readonly message?: string | null;
}

export interface add_node_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.add_node;
  readonly origin_id: number;
  readonly node: node_s;
}

export interface remove_node_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.remove_node;
  readonly origin_id: number;
  readonly id: string;
}

export interface update_node_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.update_node;
  readonly origin_id: number;
  readonly id: string;
  readonly options: options_s;
  readonly has_corrected_values: boolean;
}

export interface add_connection_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.add_connection;
  readonly origin_id: number;
  readonly connection: connection_s;
}

export interface remove_connection_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.remove_connection;
  readonly origin_id: number;
  readonly connection: connection_s;
}

export interface node_status_command_s {
  readonly action: action_e.command;
  readonly topic: topic_e.node_status;
  readonly id: string;
  readonly status: node_status_s;
}

export interface rect_s {
  readonly pos: vec2_t;
  readonly size: vec2_t;
}

export interface connected_status_s {
  readonly connected: boolean;
}

export interface device_names_status_s {
  readonly device_names: readonly settings_option_s[];
}

export interface display_modes_status_s {
  readonly display_modes: readonly settings_option_s[];
}

export interface source_names_status_s {
  readonly source_names: readonly settings_option_s[];
}

export interface monitor_options_status_s {
  readonly monitors: readonly settings_option_s[];
}

export interface font_names_status_s {
  readonly font_names: readonly settings_option_s[];
}

export interface font_variants_status_s {
  readonly font_variants: readonly settings_option_s[];
}

export interface application_frame_status_s {
  readonly frame_rate: frame_rate_s;
  readonly frame_duration_flicks: number;
  readonly epoch: number;
}

export interface application_lifecycle_status_s {
  readonly prepare_duration_us: number;
  readonly submit_duration_us: number;
  readonly execute_duration_us: number;
  readonly gpu_finish_duration_us: number;
  readonly complete_duration_us: number;
  readonly demanding_node_count: number;
  readonly submitted_node_count: number;
  readonly executed_node_count: number;
}

export interface application_scheduler_status_s {
  readonly clock_source: string;
  readonly frame_number: number;
  readonly pts_flicks: number;
  readonly render_duration_us: number;
  readonly render_duration_max_us: number;
  readonly render_duration_max_frame: number;
  readonly start_lateness_us: number;
  readonly start_lateness_max_us: number;
  readonly start_lateness_max_frame: number;
  readonly deadline_margin_us: number;
  readonly deadline_margin_min_us: number;
  readonly deadline_margin_min_frame: number;
  readonly deadline_misses_total: number;
  readonly skipped_frames_last: number;
  readonly skipped_frames_total: number;
  readonly sustained_overload: boolean;
}

export interface render_delay_test_status_s {
  readonly test_render_delay_ms: number;
  readonly test_render_delay_every: number;
  readonly test_render_delay_injections: number;
}

export interface source_timing_status_s {
  readonly source_queue_pushed: number;
  readonly source_queue_depth: number;
  readonly source_queue_overflow_drops: number;
  readonly source_queue_selection_drops: number;
  readonly source_queue_repeated: number;
  readonly source_queue_starvation_repeats: number;
  readonly source_queue_timing_repeats: number;
  readonly source_queue_missing: number;
  readonly source_queue_discontinuities: number;
  readonly source_queue_transfer_failures: number;
  readonly source_queue_transfer_cancellations: number;
  readonly source_recovered_rate?: number | null;
  readonly source_observed_rate?: number | null;
  readonly source_phase_offset_us?: number | null;
  readonly source_phase_error_us?: number | null;
  readonly source_phase_adjustment_us?: number | null;
  readonly source_repeat_next_frame_lead_min_us?: number | null;
  readonly source_repeat_next_frame_lead_max_us?: number | null;
}

export interface decklink_input_device_status_s {
  readonly signal_locked?: boolean | null;
  readonly ancillary_signal_locked?: boolean | null;
  readonly capture_busy?: boolean | null;
  readonly pcie_link_width?: number | null;
  readonly pcie_link_speed?: number | null;
  readonly temperature_c?: number | null;
  readonly active_format?: string | null;
  readonly detected_format?: string | null;
  readonly detected_colorspace?: string | null;
  readonly detected_dynamic_range?: string | null;
  readonly detected_field_dominance?: string | null;
  readonly detected_sdi_link_configuration?: string | null;
  readonly input_pixel_format?: string | null;
}

export interface decklink_output_device_status_s {
  readonly reference_locked?: boolean | null;
  readonly playback_busy?: boolean | null;
  readonly pcie_link_width?: number | null;
  readonly pcie_link_speed?: number | null;
  readonly temperature_c?: number | null;
  readonly active_format?: string | null;
  readonly output_pixel_format?: string | null;
  readonly reference_format?: string | null;
}

export interface decklink_input_metrics_status_s {
  readonly frames_received: number;
  readonly frames_missing: number;
  readonly no_input_source_frames: number;
  readonly upload_slot_drops: number;
  readonly upload_acquire_slow_count: number;
  readonly upload_acquire_failures: number;
  readonly upload_acquire_wait_max_us: number;
  readonly content_frames_sampled: number;
  readonly content_frame_repeats: number;
  readonly content_repeat_streak: number;
  readonly content_repeat_streak_max: number;
  readonly available_video_frames: number;
}

export interface ndi_input_metrics_status_s {
  readonly frames_received: number;
  readonly invalid_frames: number;
  readonly receiver_video_drops: number;
  readonly receiver_queue_depth: number;
  readonly upload_slot_drops: number;
}

export interface download_stream_status_s {
  readonly download_slots: number;
  readonly download_slots_free: number;
  readonly download_slots_rendering: number;
  readonly download_slots_queued: number;
  readonly download_slots_ready: number;
  readonly download_slots_cpu_reading: number;
  readonly download_pending_allocations: number;
  readonly download_acquire_misses: number;
  readonly download_transfers_completed: number;
  readonly download_transfer_failures: number;
  readonly download_transfer_duration_total_us: number;
  readonly download_transfer_duration_max_us: number;
  readonly download_allocation_failed: boolean;
}

export interface ndi_output_metrics_status_s {
  readonly program_frames_received: number;
  readonly program_queue_overflow_drops: number;
  readonly program_timing_drops: number;
  readonly program_frames_repeated: number;
  readonly program_frames_missing: number;
  readonly output_intervals_skipped: number;
  readonly frames_sent: number;
  readonly queued_frames: number;
  readonly output_latency_us: number;
  readonly program_selection_offset_us: number;
  readonly render_target_drops: number;
}

export interface decklink_output_metrics_status_s {
  readonly frames_completed: number;
  readonly frames_displayed_late: number;
  readonly frames_dropped: number;
  readonly frames_flushed: number;
  readonly program_frames_received: number;
  readonly program_queue_overflow_drops: number;
  readonly program_timing_drops: number;
  readonly program_frames_repeated: number;
  readonly program_frames_missing: number;
  readonly program_cadence_repeats: number;
  readonly program_starvation_repeats: number;
  readonly program_starvation_repeat_streak: number;
  readonly program_starvation_repeat_streak_max: number;
  readonly output_refill_shortfalls: number;
  readonly content_frames_sampled: number;
  readonly content_frame_repeats: number;
  readonly content_repeat_streak: number;
  readonly content_repeat_streak_max: number;
  readonly completion_intervals: number;
  readonly completion_interval_max_us: number;
  readonly program_queue_depth: number;
  readonly program_queue_depth_max: number;
  readonly buffered_video_frames: number;
  readonly buffered_video_frames_min: number;
  readonly buffered_video_frames_max: number;
  readonly buffered_below_target_samples: number;
  readonly buffered_zero_samples: number;
  readonly output_latency_us: number;
  readonly program_selection_offset_us: number;
  readonly completion_time_failures: number;
  readonly render_target_drops: number;
}

export interface screen_output_metrics_status_s {
  readonly clock_quality: string;
  readonly frames_submitted: number;
  readonly program_queue_overflow_drops: number;
  readonly program_timing_drops: number;
  readonly program_frames_repeated: number;
  readonly program_frames_missing: number;
  readonly output_intervals_skipped: number;
  readonly swaps_completed: number;
  readonly render_acquire_misses: number;
  readonly queued_frames: number;
  readonly render_slots: number;
  readonly render_slots_free: number;
  readonly render_slots_retiring: number;
  readonly output_latency_us: number;
  readonly program_selection_offset_us: number;
  readonly completion_interval_max_us: number;
  readonly measured_refresh_hz: number;
}

// A status payload is a sparse delta. The intersection forms one catalog of
// known keys; Partial makes every key optional for individual node types and updates.
// prettier-ignore
export type node_status_s = Partial<
  connected_status_s &
  device_names_status_s &
  display_modes_status_s &
  source_names_status_s &
  monitor_options_status_s &
  font_names_status_s &
  font_variants_status_s &
  application_frame_status_s &
  application_lifecycle_status_s &
  application_scheduler_status_s &
  render_delay_test_status_s &
  source_timing_status_s &
  decklink_input_device_status_s &
  decklink_output_device_status_s &
  decklink_input_metrics_status_s &
  ndi_input_metrics_status_s &
  download_stream_status_s &
  ndi_output_metrics_status_s &
  decklink_output_metrics_status_s &
  screen_output_metrics_status_s
>;
