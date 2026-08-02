#!/usr/bin/env bash

set -euo pipefail

readonly script_path=$(realpath "$0")
readonly project_dir=$(dirname "$(dirname "${script_path}")")
readonly build_dir=${BUILD_DIR:-${project_dir}/build}
readonly source_settings=${SETTINGS:-${build_dir}/settings.json}
readonly api_url=${MIXIMUS_API_URL:-http://127.0.0.1:7351/api/v1}
readonly runs_dir=${TIMING_SOAK_RUNS_DIR:-${build_dir}/integration-tests}
readonly warmup_seconds=${TIMING_SOAK_WARMUP_SECONDS:-10}
soak_app_pid=''
soak_logger_pid=''
soak_fifo=''
soak_exit_code=0
soak_observe_existing=false

timestamp()
{
    local value
    value=$(date '+%Y-%m-%d %H:%M:%S.%N')
    printf '%s\n' "${value:0:23}"
}

usage()
{
    cat <<EOF
Usage:
  $0 start [DURATION] [--decklink-output-buffer FRAMES]
                       [--decklink-display-mode MODE]
                       [--render-delay-ms MS --render-delay-every FRAMES]
                          Start a detached soak (default: 8h)
  $0 restart [DURATION] [--decklink-output-buffer FRAMES]
                          Gracefully replace one live instance with a detached soak
  $0 run DURATION DIR [FRAMES] [MODE] [DELAY_MS] [DELAY_EVERY]
                          Run the coordinator in the foreground
  $0 observe DURATION DIR
                          Sample an already-running Miximus without stopping it
  $0 status [DIR]         Show the current run state and recent events
  $0 follow [DIR]         Wait for a run to finish and report state changes
  $0 snapshot [DIR] [TYPE]
                          Show the latest sampled status, optionally by node type
  $0 live-snapshot [TYPE]
                          Show current timing status from a running Miximus instance
  $0 set-decklink-output-buffer FRAMES
                          Update the running instance's global DeckLink buffer depth
  $0 stop [DIR]           Gracefully stop a running soak
  $0 report [DIR]         Print the completed run summary
  $0 brief-report [DIR]   Print paired timing deltas and final queue state
  $0 campaign [TOTAL] [RUN]
                          Start a detached campaign (defaults: 8h, 30m)
  $0 campaign-status      Show campaign state and recent events
  $0 campaign-follow [DIR]
                          Follow campaign progress until it stops
  $0 campaign-stop        Gracefully stop the active campaign
  $0 campaign-report      Print the completed campaign summary

DURATION accepts seconds or an s, m, or h suffix. DIR defaults to
${runs_dir}/timing-soak-latest for status, stop, and report.
EOF
}

duration_seconds()
{
    local value=$1
    if [[ "${value}" =~ ^([1-9][0-9]*)([smh]?)$ ]]; then
        local amount=${BASH_REMATCH[1]}
        case ${BASH_REMATCH[2]} in
            h) echo $((amount * 3600)) ;;
            m) echo $((amount * 60)) ;;
            *) echo "${amount}" ;;
        esac
        return
    fi
    printf 'Invalid duration: %s\n' "${value}" >&2
    exit 2
}

resolve_run_dir()
{
    realpath -m "${1:-${runs_dir}/timing-soak-latest}"
}

resolve_campaign_dir()
{
    realpath -m "${1:-${runs_dir}/timing-campaign-latest}"
}

require_commands()
{
    local command
    for command in curl jq pgrep ps realpath tee; do
        if ! command -v "${command}" >/dev/null; then
            printf 'Required command not found: %s\n' "${command}" >&2
            exit 2
        fi
    done
}

start_run()
{
    local duration=8h
    local output_buffer_frames=''
    local display_mode=''
    local render_delay_ms=''
    local render_delay_every=''
    if (($# > 0)) && [[ $1 != --* ]]; then
        duration=$1
        shift
    fi
    while (($# > 0)); do
        case $1 in
            --decklink-output-buffer)
                (($# >= 2)) && [[ $2 =~ ^[1-8]$ ]] || { usage >&2; exit 2; }
                output_buffer_frames=$2
                shift 2
                ;;
            --decklink-display-mode)
                (($# >= 2)) && [[ -n $2 ]] || { usage >&2; exit 2; }
                display_mode=$2
                shift 2
                ;;
            --render-delay-ms)
                (($# >= 2)) && [[ $2 =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
                render_delay_ms=$2
                shift 2
                ;;
            --render-delay-every)
                (($# >= 2)) && [[ $2 =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
                render_delay_every=$2
                shift 2
                ;;
            *)
                usage >&2
                exit 2
                ;;
        esac
    done
    if { [[ -n ${render_delay_ms} ]] && [[ -z ${render_delay_every} ]]; } ||
        { [[ -z ${render_delay_ms} ]] && [[ -n ${render_delay_every} ]]; }; then
        printf '%s\n' '--render-delay-ms and --render-delay-every must be used together.' >&2
        exit 2
    fi
    local seconds
    seconds=$(duration_seconds "${duration}")
    require_commands

    if curl -fsS "${api_url}/config" >/dev/null 2>&1; then
        printf 'Miximus is already serving %s; refusing to start a second instance.\n' "${api_url}" >&2
        exit 1
    fi
    if [[ ! -x "${build_dir}/miximus" ]]; then
        printf 'Miximus executable not found: %s\n' "${build_dir}/miximus" >&2
        exit 2
    fi
    if [[ ! -f "${source_settings}" ]]; then
        printf 'Settings file not found: %s\n' "${source_settings}" >&2
        exit 2
    fi

    mkdir -p "${runs_dir}"
    local run_dir="${runs_dir}/timing-soak-$(date +%Y%m%d-%H%M%S)"
    mkdir "${run_dir}"
    ln -sfn "$(basename "${run_dir}")" "${runs_dir}/timing-soak-latest"

    if systemctl --user show-environment >/dev/null 2>&1; then
        local unit="miximus-timing-soak-$(date +%Y%m%d-%H%M%S)"
        local -a environment=(
            "--setenv=BUILD_DIR=${build_dir}"
            "--setenv=SETTINGS=${source_settings}"
            "--setenv=MIXIMUS_API_URL=${api_url}"
            "--setenv=TIMING_SOAK_RUNS_DIR=${runs_dir}"
            "--setenv=TIMING_SOAK_WARMUP_SECONDS=${warmup_seconds}"
        )
        local name
        for name in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR; do
            if [[ -v ${name} ]]; then
                environment+=("--setenv=${name}=${!name}")
            fi
        done

        printf '%s\n' "${unit}" >"${run_dir}/systemd-unit"
        systemd-run --user --collect --quiet --unit "${unit}" --property="WorkingDirectory=${project_dir}" \
            "${environment[@]}" "${script_path}" run "${seconds}" "${run_dir}" \
            "${output_buffer_frames}" "${display_mode}" "${render_delay_ms}" "${render_delay_every}"
        printf 'Started timing soak %s for %ss (user service %s).\n' "${run_dir}" "${seconds}" "${unit}"
    else
        nohup "${script_path}" run "${seconds}" "${run_dir}" "${output_buffer_frames}" "${display_mode}" \
            "${render_delay_ms}" "${render_delay_every}" \
            >"${run_dir}/bootstrap.log" 2>&1 &
        local runner_pid=$!
        printf '%s\n' "${runner_pid}" >"${run_dir}/runner.pid"
        printf 'Started timing soak %s for %ss (runner PID %s).\n' "${run_dir}" "${seconds}" "${runner_pid}"
    fi
}

stop_live_instance()
{
    if ! curl -fsS "${api_url}/config" >/dev/null 2>&1; then
        return
    fi

    local -a live_pids=()
    mapfile -t live_pids < <(pgrep -x miximus 2>/dev/null || true)
    if ((${#live_pids[@]} != 1)); then
        printf 'Expected exactly one live Miximus process, found %s; refusing to stop anything.\n' \
            "${#live_pids[@]}" >&2
        exit 1
    fi

    local pid=${live_pids[0]}
    printf 'Requesting graceful shutdown of live Miximus process %s.\n' "${pid}"
    kill -INT "${pid}"
    for _ in {1..600}; do
        if ! kill -0 "${pid}" 2>/dev/null && ! curl -fsS "${api_url}/config" >/dev/null 2>&1; then
            return
        fi
        sleep 0.1
    done

    printf 'Live Miximus process %s did not stop within 60 seconds.\n' "${pid}" >&2
    exit 1
}

restart_run()
{
    stop_live_instance
    start_run "$@"
}

run_soak()
{
    local seconds=$1
    local run_dir=$2
    local output_buffer_frames=${3:-}
    local display_mode=${4:-}
    soak_observe_existing=${5:-false}
    local render_delay_ms=${6:-}
    local render_delay_every=${7:-}
    require_commands
    mkdir -p "${run_dir}"

    local events_file="${run_dir}/events.log"
    local samples_file="${run_dir}/status-samples.jsonl"
    local system_file="${run_dir}/system-samples.jsonl"
    local settings_file="${run_dir}/settings.json"
    soak_fifo="${run_dir}/app-output.fifo"
    soak_app_pid=''
    soak_logger_pid=''
    local stopping=false
    soak_exit_code=0

    app_running()
    {
        if [[ "${soak_observe_existing}" == true ]]; then
            if [[ -n "${soak_app_pid}" ]]; then
                kill -0 "${soak_app_pid}" 2>/dev/null &&
                    [[ $(ps -p "${soak_app_pid}" -o stat= 2>/dev/null) != Z* ]]
                return
            fi
            curl -fsS "${api_url}/config" >/dev/null 2>&1
            return
        fi
        [[ -n "${soak_app_pid}" ]] && kill -0 "${soak_app_pid}" 2>/dev/null &&
            [[ $(ps -p "${soak_app_pid}" -o stat= 2>/dev/null) != Z* ]]
    }

    log_event()
    {
        local level=$1
        shift
        printf '[%s] [soak] [%s] %s\n' "$(timestamp)" "${level}" "$*" | tee -a "${events_file}"
    }

    request_stop()
    {
        stopping=true
        log_event info 'Stop requested'
    }

    cleanup()
    {
        trap - EXIT INT TERM
        if [[ "${soak_observe_existing}" != true ]] && app_running; then
            kill -INT "${soak_app_pid}" 2>/dev/null || true
            wait "${soak_app_pid}" || soak_exit_code=$?
        fi
        if [[ -n "${soak_logger_pid}" ]]; then
            wait "${soak_logger_pid}" 2>/dev/null || true
        fi
        rm -f "${soak_fifo}"
        if [[ $(cat "${run_dir}/state" 2>/dev/null || true) == running ]]; then
            printf 'failed\n' >"${run_dir}/state"
        fi
    }
    trap request_stop INT TERM
    trap cleanup EXIT

    printf 'starting\n' >"${run_dir}/state"
    printf '%s\n' "$$" >"${run_dir}/runner.pid"
    rm -f "${run_dir}/summary.json" "${run_dir}/config.json" "${run_dir}/app.pid" "${soak_fifo}"
    : >"${events_file}"
    : >"${samples_file}"
    : >"${system_file}"

    if [[ "${soak_observe_existing}" == true ]]; then
        soak_app_pid=$(pgrep -n -x miximus 2>/dev/null || true)
        if [[ -n "${soak_app_pid}" ]]; then
            printf '%s\n' "${soak_app_pid}" >"${run_dir}/app.pid"
        fi
        log_event info "Observing the running Miximus instance for ${seconds}s"
    else
        cp "${source_settings}" "${settings_file}"
        if [[ -n "${output_buffer_frames}" ]]; then
            local rewritten_settings="${settings_file}.tmp"
            jq --argjson frames "${output_buffer_frames}" '
                (.nodes[] | select(.id == "$app").options.decklink_output_buffer_frames) = $frames' \
                "${settings_file}" >"${rewritten_settings}"
            mv "${rewritten_settings}" "${settings_file}"
        fi
        if [[ -n "${display_mode}" ]]; then
            local rewritten_settings="${settings_file}.tmp"
            jq --arg mode "${display_mode}" '
                (.nodes[] | select(.type == "decklink_output").options.display_mode) = $mode' \
                "${settings_file}" >"${rewritten_settings}"
            mv "${rewritten_settings}" "${settings_file}"
        fi
        mkfifo "${soak_fifo}"

        while IFS= read -r line; do
            printf '[app] %s\n' "${line}" | tee -a "${events_file}"
        done <"${soak_fifo}" &
        soak_logger_pid=$!

        local -a app_arguments=(--settings "${settings_file}" --stop-after "$((seconds + 60))")
        if [[ -n ${render_delay_ms} ]]; then
            app_arguments+=(--test-render-delay-ms "${render_delay_ms}" --test-render-delay-every "${render_delay_every}")
        fi
        log_event info "Starting Miximus for ${seconds}s with ${settings_file}"
        "${build_dir}/miximus" "${app_arguments[@]}" >"${soak_fifo}" 2>&1 &
        soak_app_pid=$!
        printf '%s\n' "${soak_app_pid}" >"${run_dir}/app.pid"
    fi

    local ready=false
    for _ in {1..300}; do
        if curl -fsS "${api_url}/config" >"${run_dir}/config.json" 2>/dev/null; then
            ready=true
            break
        fi
        if ! app_running; then
            break
        fi
        sleep 0.1
    done
    if [[ "${ready}" != true ]]; then
        log_event error 'Miximus API did not become ready'
        printf 'failed\n' >"${run_dir}/state"
        return 1
    fi

    local monitored_nodes
    monitored_nodes=$(jq -c \
        '[.nodes[] | select(.id == "$app" or (.type | IN("decklink_input", "decklink_output", "ndi_input", "ndi_output", "screen_output"))) | {id, type}]' \
        "${run_dir}/config.json")
    log_event info "Monitoring $(jq 'length' <<<"${monitored_nodes}") timing-relevant node(s)"
    printf 'running\n' >"${run_dir}/state"

    declare -A previous_counters=()
    declare -A increasing_counters=()
    local -a anomaly_keys=(
        deadline_misses_total
        skipped_frames_total
        frames_displayed_late
        frames_dropped
        completion_time_failures
        program_queue_overflow_drops
        program_timing_drops
        program_starvation_repeats
        output_refill_shortfalls
        buffered_zero_samples
        render_target_drops
        download_acquire_misses
        download_transfer_failures
        upload_slot_drops
        upload_acquire_failures
        source_queue_overflow_drops
        source_queue_selection_drops
        source_queue_starvation_repeats
        source_queue_timing_repeats
        source_queue_transfer_failures
        program_frames_missing
        program_starvation_repeat_streak_max
        buffered_below_target_samples
        output_intervals_skipped
        render_acquire_misses
        receiver_video_drops
        invalid_frames
    )
    local anomaly_keys_json
    anomaly_keys_json=$(printf '%s\n' "${anomaly_keys[@]}" | jq -R . | jq -sc .)

    local start_epoch_ns
    start_epoch_ns=$(date +%s%N)
    local next_gpu_sample=0
    local reached_duration=false
    while [[ "${stopping}" != true ]] && app_running; do
        local now_epoch_ns elapsed wall_time
        now_epoch_ns=$(date +%s%N)
        elapsed=$(((now_epoch_ns - start_epoch_ns) / 1000000000))
        wall_time=$(date --iso-8601=ns)
        if ((elapsed >= seconds)); then
            reached_duration=true
            log_event info "Reached requested runtime of ${seconds}s"
            break
        fi

        local process_sample
        process_sample=$(ps -p "${soak_app_pid}" -o %cpu=,rss=,nlwp= 2>/dev/null | awk '{$1=$1; print}' || true)
        if [[ -n "${process_sample}" ]]; then
            read -r cpu rss threads <<<"${process_sample}"
            jq -nc --arg time "${wall_time}" --argjson elapsed "${elapsed}" --argjson cpu "${cpu}" \
                --argjson rss_kib "${rss}" --argjson threads "${threads}" \
                '{timestamp: $time, elapsed_seconds: $elapsed, cpu_percent: $cpu, rss_kib: $rss_kib, threads: $threads}' \
                >>"${system_file}"
        fi

        if command -v nvidia-smi >/dev/null && ((elapsed >= next_gpu_sample)); then
            local gpu_sample
            gpu_sample=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null |
                awk -F, -v pid="${soak_app_pid}" '$1 + 0 == pid {gsub(/ /, "", $2); print $2; exit}' || true)
            if [[ -n "${gpu_sample}" ]]; then
                jq -nc --arg time "${wall_time}" --argjson elapsed "${elapsed}" --argjson gpu_memory_mib "${gpu_sample}" \
                    '{timestamp: $time, elapsed_seconds: $elapsed, gpu_memory_mib: $gpu_memory_mib}' >>"${system_file}"
            fi
            next_gpu_sample=$((elapsed + 10))
        fi

        local all_status
        if ! all_status=$(curl -fsS "${api_url}/status" 2>/dev/null); then
            log_event warning 'Failed to query aggregate node status'
            sleep 1 || true
            continue
        fi

        jq -c --arg time "${wall_time}" --argjson elapsed "${elapsed}" --argjson nodes "${monitored_nodes}" '
            $nodes[] as $node
            | {
                timestamp: $time,
                elapsed_seconds: $elapsed,
                node_id: $node.id,
                node_type: $node.type,
                status: (.[$node.id] // {})
            }' <<<"${all_status}" >>"${samples_file}"

        while IFS=$'\t' read -r node_id node_type key value; do
            local counter_id="${node_id}:${key}"
            local previous=${previous_counters[${counter_id}]:-${value}}
            if ((elapsed >= warmup_seconds && value > previous)); then
                if [[ ${increasing_counters[${counter_id}]:-0} != 1 ]]; then
                    log_event warning "${node_type} ${node_id}: ${key} started increasing (${previous} -> ${value})"
                fi
                increasing_counters[${counter_id}]=1
            elif ((value < previous)); then
                log_event info "${node_type} ${node_id}: ${key} reset ${previous} -> ${value}"
                increasing_counters[${counter_id}]=0
            else
                increasing_counters[${counter_id}]=0
            fi
            previous_counters[${counter_id}]=${value}
        done < <(jq -r --argjson nodes "${monitored_nodes}" --argjson keys "${anomaly_keys_json}" '
            $nodes[] as $node
            | (.[$node.id] // {})
            | to_entries[]
            | select(.key as $key | $keys | index($key))
            | select(.value | type == "number")
            | [$node.id, $node.type, .key, .value]
            | @tsv' <<<"${all_status}")
        sleep 1 || true
    done

    if [[ "${soak_observe_existing}" == true && "${reached_duration}" != true && "${stopping}" != true ]]; then
        soak_exit_code=1
        log_event error 'Observed Miximus instance stopped before the requested runtime'
    fi

    if [[ "${soak_observe_existing}" != true ]] && app_running; then
        kill -INT "${soak_app_pid}"
    fi
    if [[ "${soak_observe_existing}" != true ]]; then
        wait "${soak_app_pid}" || soak_exit_code=$?
    fi
    soak_app_pid=''
    wait "${soak_logger_pid}" 2>/dev/null || true
    soak_logger_pid=''
    rm -f "${soak_fifo}"

    local end_epoch_ns
    end_epoch_ns=$(date +%s%N)
    jq -n --slurpfile samples "${samples_file}" --slurpfile system "${system_file}" \
        --argjson anomaly_keys "${anomaly_keys_json}" --argjson requested_seconds "${seconds}" \
        --argjson warmup_seconds "${warmup_seconds}" \
        --arg output_buffer_frames "${output_buffer_frames}" \
        --arg display_mode "${display_mode}" \
        --arg render_delay_ms "${render_delay_ms}" \
        --arg render_delay_every "${render_delay_every}" \
        --argjson elapsed_seconds "$(((end_epoch_ns - start_epoch_ns) / 1000000000))" \
        --argjson app_exit_code "${soak_exit_code}" '
        {
            requested_seconds: $requested_seconds,
            elapsed_seconds: $elapsed_seconds,
            app_exit_code: $app_exit_code,
            decklink_output_buffer_frames_override:
                (if $output_buffer_frames == "" then null else ($output_buffer_frames | tonumber) end),
            decklink_display_mode_override:
                (if $display_mode == "" then null else $display_mode end),
            render_delay_ms:
                (if $render_delay_ms == "" then null else ($render_delay_ms | tonumber) end),
            render_delay_every:
                (if $render_delay_every == "" then null else ($render_delay_every | tonumber) end),
            test_render_delay_injections:
                ([$samples[]
                    | select(.elapsed_seconds >= $warmup_seconds and .node_type == "application_settings")
                    | .status.test_render_delay_injections | select(type == "number")] as $values
                    | if ($values | length) > 0 then $values[-1] - $values[0] else null end),
            warmup_seconds: $warmup_seconds,
            system: {
                cpu_percent_max: ([$system[] | .cpu_percent // empty] | max // null),
                rss_kib_first: ([$system[] | .rss_kib // empty] | first // null),
                rss_kib_last: ([$system[] | .rss_kib // empty] | last // null),
                rss_kib_min: ([$system[] | .rss_kib // empty] | min // null),
                rss_kib_max: ([$system[] | .rss_kib // empty] | max // null),
                rss_kib_steady_first:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .rss_kib // empty] | first // null),
                rss_kib_steady_last:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .rss_kib // empty] | last // null),
                rss_kib_steady_min:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .rss_kib // empty] | min // null),
                rss_kib_steady_max:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .rss_kib // empty] | max // null),
                threads_max: ([$system[] | .threads // empty] | max // null),
                gpu_memory_mib_first: ([$system[] | .gpu_memory_mib // empty] | first // null),
                gpu_memory_mib_last: ([$system[] | .gpu_memory_mib // empty] | last // null),
                gpu_memory_mib_min: ([$system[] | .gpu_memory_mib // empty] | min // null),
                gpu_memory_mib_max: ([$system[] | .gpu_memory_mib // empty] | max // null),
                gpu_memory_mib_steady_first:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .gpu_memory_mib // empty]
                        | first // null),
                gpu_memory_mib_steady_last:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .gpu_memory_mib // empty]
                        | last // null),
                gpu_memory_mib_steady_min:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .gpu_memory_mib // empty]
                        | min // null),
                gpu_memory_mib_steady_max:
                    ([$system[] | select(.elapsed_seconds >= $warmup_seconds) | .gpu_memory_mib // empty]
                        | max // null)
            },
            nodes: ($samples
                | group_by(.node_id)
                | map(. as $node
                    | ($node | map(select(.elapsed_seconds >= $warmup_seconds))) as $steady_samples
                    | (if ($steady_samples | length) > 0 then $steady_samples else $node end) as $steady
                    | {
                    id: $node[0].node_id,
                    type: $node[0].node_type,
                    first_status: $node[0].status,
                    warmup_status: $steady[0].status,
                    last_status: $node[-1].status,
                    anomaly_counter_deltas: (reduce $anomaly_keys[] as $key ({};
                        [$steady[].status[$key] | select(type == "number")] as $values
                        | if ($values | length) > 0
                          then .[$key] = $values[-1] - $values[0]
                          else . end))
                })),
            steady_scheduler: {
                render_duration_max_us:
                    ([$samples[]
                        | select(.elapsed_seconds >= $warmup_seconds and .node_type == "application_settings")
                        | .status.render_duration_us | select(type == "number")] | max // null),
                start_lateness_max_us:
                    ([$samples[]
                        | select(.elapsed_seconds >= $warmup_seconds and .node_type == "application_settings")
                        | .status.start_lateness_us | select(type == "number")] | max // null),
                deadline_margin_min_us:
                    ([$samples[]
                        | select(.elapsed_seconds >= $warmup_seconds and .node_type == "application_settings")
                        | .status.deadline_margin_us | select(type == "number")] | min // null)
            }
        }' >"${run_dir}/summary.json"

    if ((soak_exit_code == 0)); then
        printf 'completed\n' >"${run_dir}/state"
        log_event info "Timing soak completed. Summary: ${run_dir}/summary.json"
    else
        printf 'failed\n' >"${run_dir}/state"
        log_event error "Miximus exited with status ${soak_exit_code}"
    fi
    trap - EXIT INT TERM
    return "${soak_exit_code}"
}

start_campaign()
{
    local total=${1:-8h}
    local run_duration=${2:-30m}
    (($# <= 2)) || { usage >&2; exit 2; }

    local total_seconds run_seconds
    total_seconds=$(duration_seconds "${total}")
    run_seconds=$(duration_seconds "${run_duration}")
    if ((run_seconds > total_seconds)); then
        printf 'Campaign run duration cannot exceed total duration.\n' >&2
        exit 2
    fi
    require_commands

    if curl -fsS "${api_url}/config" >/dev/null 2>&1; then
        printf 'Miximus is already serving %s; refusing to start a campaign.\n' "${api_url}" >&2
        exit 1
    fi
    if [[ ! -x "${build_dir}/miximus" || ! -f "${source_settings}" ]]; then
        printf 'Campaign requires %s and %s.\n' "${build_dir}/miximus" "${source_settings}" >&2
        exit 2
    fi

    mkdir -p "${runs_dir}"
    local campaign_dir="${runs_dir}/timing-campaign-$(date +%Y%m%d-%H%M%S)"
    mkdir "${campaign_dir}"
    ln -sfn "$(basename "${campaign_dir}")" "${runs_dir}/timing-campaign-latest"

    if systemctl --user show-environment >/dev/null 2>&1; then
        local unit="miximus-timing-campaign-$(date +%Y%m%d-%H%M%S)"
        local -a environment=(
            "--setenv=BUILD_DIR=${build_dir}"
            "--setenv=SETTINGS=${source_settings}"
            "--setenv=MIXIMUS_API_URL=${api_url}"
            "--setenv=TIMING_SOAK_RUNS_DIR=${runs_dir}"
            "--setenv=TIMING_SOAK_WARMUP_SECONDS=${warmup_seconds}"
        )
        local name
        for name in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR; do
            if [[ -v ${name} ]]; then
                environment+=("--setenv=${name}=${!name}")
            fi
        done

        printf '%s\n' "${unit}" >"${campaign_dir}/systemd-unit"
        systemd-run --user --collect --quiet --unit "${unit}" --property="WorkingDirectory=${project_dir}" \
            "${environment[@]}" "${script_path}" campaign-run "${total_seconds}" "${run_seconds}" "${campaign_dir}"
        printf 'Started timing campaign %s (user service %s).\n' "${campaign_dir}" "${unit}"
    else
        nohup "${script_path}" campaign-run "${total_seconds}" "${run_seconds}" "${campaign_dir}" \
            >"${campaign_dir}/bootstrap.log" 2>&1 &
        printf '%s\n' "$!" >"${campaign_dir}/runner.pid"
        printf 'Started timing campaign %s.\n' "${campaign_dir}"
    fi
}

run_campaign()
{
    local total_seconds=$1
    local run_seconds=$2
    local campaign_dir=$3
    local events_file="${campaign_dir}/events.log"
    local summaries_file="${campaign_dir}/run-summaries.jsonl"
    local child_pid=''
    local stop_requested=false

    campaign_log()
    {
        local level=$1
        shift
        printf '[%s] [campaign] [%s] %s\n' "$(timestamp)" "${level}" "$*" | tee -a "${events_file}"
    }

    stop_campaign_child()
    {
        stop_requested=true
        campaign_log info 'Stop requested'
        if [[ -n "${child_pid}" ]] && kill -0 "${child_pid}" 2>/dev/null; then
            kill -INT "${child_pid}" 2>/dev/null || true
        fi
    }
    trap stop_campaign_child INT TERM

    mkdir -p "${campaign_dir}"
    printf '%s\n' "$$" >"${campaign_dir}/runner.pid"
    printf 'running\n' >"${campaign_dir}/state"
    : >"${events_file}"
    : >"${summaries_file}"

    local elapsed=0
    local index=0
    while ((elapsed < total_seconds)) && [[ "${stop_requested}" != true ]]; do
        local duration=${run_seconds}
        if ((elapsed + duration > total_seconds)); then
            duration=$((total_seconds - elapsed))
        fi

        local sequence_index=$((index % 4))
        local buffer_frames=5
        if ((sequence_index == 1 || sequence_index == 2)); then
            buffer_frames=4
        fi

        local run_number=$((index + 1))
        local run_dir
        run_dir=$(printf '%s/run-%02d-buffer-%d' "${campaign_dir}" "${run_number}" "${buffer_frames}")
        mkdir -p "${run_dir}"
        campaign_log info "Starting run ${run_number}: ${duration}s, DeckLink output buffer ${buffer_frames}"

        "${script_path}" run "${duration}" "${run_dir}" "${buffer_frames}" >"${run_dir}/coordinator.log" 2>&1 &
        child_pid=$!
        local child_status=0
        wait "${child_pid}" || child_status=$?
        child_pid=''

        if [[ ! -s "${run_dir}/summary.json" ]]; then
            if [[ "${stop_requested}" == true ]]; then
                campaign_log info "Interrupted run ${run_number} without retaining a partial sample"
                break
            fi
            campaign_log error "Run ${run_number} did not produce a summary (status ${child_status})"
            printf 'failed\n' >"${campaign_dir}/state"
            trap - INT TERM
            return 1
        fi

        jq -c --argjson run_number "${run_number}" --argjson buffer_frames "${buffer_frames}" \
            '{run_number: $run_number, buffer_frames: $buffer_frames, summary: .}' "${run_dir}/summary.json" \
            >>"${summaries_file}"
        if ((child_status != 0)); then
            campaign_log warning "Completed run ${run_number} with status ${child_status}; retaining the sample"
        else
            campaign_log info "Completed run ${run_number} with status 0"
        fi

        elapsed=$((elapsed + duration))
        index=$((index + 1))
    done

    jq -s --argjson requested_seconds "${total_seconds}" --argjson run_seconds "${run_seconds}" '
        def counter_delta($node; $key):
            ($node.warmup_status[$key] // 0) as $first
            | ($node.last_status[$key] // $first) - $first;
        {
            requested_seconds: $requested_seconds,
            configured_run_seconds: $run_seconds,
            completed_runs: length,
            nonzero_exit_runs: ([.[] | select(.summary.app_exit_code != 0)] | length),
            incomplete_runs:
                ([.[] | select(.summary.elapsed_seconds < .summary.requested_seconds)] | length),
            groups: (group_by(.buffer_frames) | map(. as $runs | {
                buffer_frames: $runs[0].buffer_frames,
                runs: ($runs | length),
                decklink_output: {
                    displayed_late_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "frames_displayed_late")] | add // 0),
                    dropped_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "frames_dropped")] | add // 0),
                    timing_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "program_timing_drops")] | add // 0),
                    frames_repeated_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "program_frames_repeated")] | add // 0),
                    content_frame_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "content_frame_repeats")] | add // 0),
                    starvation_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "program_starvation_repeats")] | add // 0),
                    refill_shortfalls_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "output_refill_shortfalls")] | add // 0),
                    buffered_zero_samples_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "buffered_zero_samples")] | add // 0),
                    buffered_below_target_samples_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "buffered_below_target_samples")] | add // 0),
                    render_target_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "render_target_drops")] | add // 0),
                    download_acquire_misses_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "download_acquire_misses")] | add // 0),
                    download_transfer_failures_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | counter_delta(.; "download_transfer_failures")] | add // 0),
                    download_transfer_duration_max_us:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | .last_status.download_transfer_duration_max_us | select(type == "number")] | max // null),
                    completion_interval_max_us:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | .last_status.completion_interval_max_us | select(type == "number")] | max // null),
                    program_queue_depth_max:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_output")
                            | .last_status.program_queue_depth_max | select(type == "number")] | max // null)
                },
                decklink_input: {
                    frames_received_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "frames_received")] | add // 0),
                    frames_missing_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "frames_missing")] | add // 0),
                    content_frame_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "content_frame_repeats")] | add // 0),
                    upload_slot_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "upload_slot_drops")] | add // 0),
                    upload_acquire_slow_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "upload_acquire_slow_count")] | add // 0),
                    upload_acquire_failures_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "upload_acquire_failures")] | add // 0),
                    upload_acquire_wait_max_us:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | .last_status.upload_acquire_wait_max_us | select(type == "number")] | max // null),
                    source_selection_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "source_queue_selection_drops")] | add // 0),
                    source_starvation_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "source_queue_starvation_repeats")] | add // 0),
                    source_timing_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "source_queue_timing_repeats")] | add // 0),
                    source_overflow_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "source_queue_overflow_drops")] | add // 0),
                    source_transfer_failures_total:
                        ([$runs[].summary.nodes[] | select(.type == "decklink_input")
                            | counter_delta(.; "source_queue_transfer_failures")] | add // 0)
                },
                ndi_input: {
                    frames_received_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "frames_received")] | add // 0),
                    invalid_frames_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "invalid_frames")] | add // 0),
                    receiver_video_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "receiver_video_drops")] | add // 0),
                    upload_slot_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "upload_slot_drops")] | add // 0),
                    source_selection_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "source_queue_selection_drops")] | add // 0),
                    source_starvation_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "source_queue_starvation_repeats")] | add // 0),
                    source_timing_repeats_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "source_queue_timing_repeats")] | add // 0),
                    source_overflow_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "source_queue_overflow_drops")] | add // 0),
                    source_transfer_failures_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_input")
                            | counter_delta(.; "source_queue_transfer_failures")] | add // 0)
                },
                ndi_output: {
                    queue_overflow_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "program_queue_overflow_drops")] | add // 0),
                    timing_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "program_timing_drops")] | add // 0),
                    frames_repeated_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "program_frames_repeated")] | add // 0),
                    frames_missing_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "program_frames_missing")] | add // 0),
                    output_intervals_skipped_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "output_intervals_skipped")] | add // 0),
                    frames_sent_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "frames_sent")] | add // 0),
                    render_target_drops_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "render_target_drops")] | add // 0),
                    download_acquire_misses_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "download_acquire_misses")] | add // 0),
                    download_transfer_failures_total:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | counter_delta(.; "download_transfer_failures")] | add // 0),
                    download_transfer_duration_max_us:
                        ([$runs[].summary.nodes[] | select(.type == "ndi_output")
                            | .last_status.download_transfer_duration_max_us | select(type == "number")] | max // null)
                },
                scheduler: {
                    deadline_misses_total:
                        ([$runs[].summary.nodes[] | select(.type == "application_settings")
                            | counter_delta(.; "deadline_misses_total")] | add // 0),
                    skipped_frames_total:
                        ([$runs[].summary.nodes[] | select(.type == "application_settings")
                            | counter_delta(.; "skipped_frames_total")] | add // 0),
                    render_duration_max_us:
                        ([$runs[].summary.steady_scheduler.render_duration_max_us | select(type == "number")]
                            | max // null),
                    start_lateness_max_us:
                        ([$runs[].summary.steady_scheduler.start_lateness_max_us | select(type == "number")]
                            | max // null),
                    deadline_margin_min_us:
                        ([$runs[].summary.steady_scheduler.deadline_margin_min_us | select(type == "number")]
                            | min // null)
                },
                process: {
                    rss_kib_max: ([$runs[].summary.system.rss_kib_max | select(type == "number")] | max // null),
                    rss_kib_growth_max:
                        ([$runs[].summary.system
                            | select(.rss_kib_steady_first | type == "number")
                            | select(.rss_kib_steady_last | type == "number")
                            | .rss_kib_steady_last - .rss_kib_steady_first] | max // null),
                    gpu_memory_mib_max:
                        ([$runs[].summary.system.gpu_memory_mib_max | select(type == "number")] | max // null),
                    gpu_memory_mib_growth_max:
                        ([$runs[].summary.system
                            | select(.gpu_memory_mib_steady_first | type == "number")
                            | select(.gpu_memory_mib_steady_last | type == "number")
                            | .gpu_memory_mib_steady_last - .gpu_memory_mib_steady_first] | max // null)
                }
            }))
        }' "${summaries_file}" >"${campaign_dir}/summary.json"

    if [[ "${stop_requested}" == true ]]; then
        printf 'stopped\n' >"${campaign_dir}/state"
        campaign_log info "Campaign stopped after ${index} completed run(s)"
    else
        printf 'completed\n' >"${campaign_dir}/state"
        campaign_log info "Campaign completed ${index} run(s). Summary: ${campaign_dir}/summary.json"
    fi
    trap - INT TERM
}

show_status()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    if [[ ! -d "${run_dir}" ]]; then
        printf 'Timing soak directory not found: %s\n' "${run_dir}" >&2
        exit 1
    fi
    printf 'Run: %s\nState: %s\n' "${run_dir}" "$(cat "${run_dir}/state" 2>/dev/null || echo starting)"
    if [[ -f "${run_dir}/systemd-unit" ]]; then
        local unit
        local service_state
        unit=$(cat "${run_dir}/systemd-unit")
        if ! service_state=$(systemctl --user is-active "${unit}" 2>/dev/null); then
            service_state=stopped
        fi
        printf 'User service: %s (%s)\n' "${unit}" "${service_state}"
    elif [[ -f "${run_dir}/runner.pid" ]]; then
        local pid
        pid=$(cat "${run_dir}/runner.pid")
        printf 'Runner PID: %s (%s)\n' "${pid}" "$(kill -0 "${pid}" 2>/dev/null && echo running || echo stopped)"
    fi
    tail -n 20 "${run_dir}/events.log" 2>/dev/null || tail -n 20 "${run_dir}/bootstrap.log" 2>/dev/null || true
}

show_snapshot()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    local samples_file="${run_dir}/status-samples.jsonl"
    if [[ ! -s "${samples_file}" ]]; then
        printf 'Timing soak has no status samples: %s\n' "${run_dir}" >&2
        exit 1
    fi

    jq -s --arg type "${2:-}" '
        group_by(.node_id)
        | map(last | select($type == "" or .node_type == $type))
        | map({id: .node_id, type: .node_type, elapsed_seconds, status})
    ' "${samples_file}"
}

show_live_snapshot()
{
    require_commands

    local config
    local all_status
    config=$(curl -fsS "${api_url}/config")
    all_status=$(curl -fsS "${api_url}/status")

    jq -n --arg type "${1:-}" --argjson config "${config}" --argjson status "${all_status}" '
        $config.nodes
        | map(select(.id == "$app" or (.type | IN("decklink_input", "decklink_output", "ndi_input", "ndi_output", "screen_output"))))
        | map(select($type == "" or .type == $type))
        | map({id, type, options, status: ($status[.id] // {})})
    '
}

set_decklink_output_buffer()
{
    local frames=$1
    if [[ ! "${frames}" =~ ^[1-8]$ ]]; then
        printf 'DeckLink output buffer depth must be between 1 and 8.\n' >&2
        exit 2
    fi
    require_commands

    local payload
    payload=$(jq -nc --argjson frames "${frames}" '{
        action: "command",
        topic: "update_node",
        id: "$app",
        options: {decklink_output_buffer_frames: $frames}
    }')
    curl -fsS -X POST -H 'Content-Type: application/json' --data-binary "${payload}" "${api_url}/control" >/dev/null
    printf 'Updated DeckLink output buffer depth to %s.\n' "${frames}"
}

stop_run()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    if [[ -f "${run_dir}/systemd-unit" ]]; then
        local unit
        unit=$(cat "${run_dir}/systemd-unit")
        if ! systemctl --user is-active --quiet "${unit}"; then
            printf 'Timing soak is not running: %s\n' "${run_dir}" >&2
            exit 1
        fi
        systemctl --user kill --kill-whom=main --signal=SIGINT "${unit}"
        printf 'Requested graceful stop of timing soak %s.\n' "${run_dir}"
        return
    fi

    local pid_file="${run_dir}/runner.pid"
    if [[ ! -f "${pid_file}" ]]; then
        printf 'Runner PID not found: %s\n' "${pid_file}" >&2
        exit 1
    fi
    local pid
    pid=$(cat "${pid_file}")
    if ! kill -0 "${pid}" 2>/dev/null; then
        printf 'Timing soak is not running: %s\n' "${run_dir}" >&2
        exit 1
    fi
    kill -INT "${pid}"
    printf 'Requested graceful stop of timing soak %s.\n' "${run_dir}"
}

show_report()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    jq . "${run_dir}/summary.json"
}

follow_run()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    local previous_state=''
    while true; do
        local state
        state=$(cat "${run_dir}/state" 2>/dev/null || echo starting)
        if [[ "${state}" != "${previous_state}" ]]; then
            printf '[%s] Timing soak %s.\n' "$(timestamp)" "${state}"
            previous_state=${state}
        fi
        case ${state} in
            completed | stopped)
                return
                ;;
            failed)
                return 1
                ;;
        esac
        sleep 30
    done
}

show_brief_report()
{
    local run_dir
    run_dir=$(resolve_run_dir "${1:-}")
    jq '
        def selected_status:
            with_entries(select(.key | IN(
                "frames_received",
                "frames_missing",
                "content_frames_sampled",
                "content_frame_repeats",
                "content_repeat_streak",
                "content_repeat_streak_max",
                "upload_slot_drops",
                "upload_acquire_failures",
                "source_queue_depth",
                "source_queue_selection_drops",
                "source_queue_starvation_repeats",
                "source_queue_timing_repeats",
                "source_queue_transfer_failures",
                "program_queue_depth",
                "program_timing_drops",
                "program_starvation_repeats",
                "program_cadence_repeats",
                "frames_displayed_late",
                "frames_dropped",
                "buffered_video_frames",
                "buffered_video_frames_min",
                "buffered_video_frames_max",
                "buffered_below_target_samples",
                "buffered_zero_samples",
                "output_refill_shortfalls",
                "swaps_completed",
                "measured_refresh_hz",
                "queued_frames",
                "render_slots",
                "render_slots_free",
                "render_slots_retiring",
                "render_acquire_misses",
                "completion_interval_max_us",
                "render_target_drops",
                "download_slots",
                "download_slots_free",
                "download_slots_rendering",
                "download_slots_queued",
                "download_slots_ready",
                "download_slots_cpu_reading",
                "download_pending_allocations",
                "download_acquire_misses",
                "download_transfers_completed",
                "download_transfer_failures",
                "download_transfer_duration_max_us",
                "download_allocation_failed",
                "frame_number",
                "start_lateness_max_us",
                "deadline_margin_min_us",
                "deadline_misses_total",
                "skipped_frames_total",
                "test_render_delay_ms",
                "test_render_delay_every",
                "test_render_delay_injections"
            )));
        {
            requested_seconds,
            elapsed_seconds,
            app_exit_code,
            decklink_output_buffer_frames_override,
            decklink_display_mode_override,
            render_delay_ms,
            render_delay_every,
            test_render_delay_injections,
            system,
            steady_scheduler,
            nodes: [.nodes[] | {
                id,
                type,
                anomaly_counter_deltas,
                last_status: (.last_status | selected_status)
            }]
        }' "${run_dir}/summary.json"
}

follow_campaign()
{
    local campaign_dir
    campaign_dir=$(resolve_campaign_dir "${1:-}")
    if [[ ! -d "${campaign_dir}" ]]; then
        printf 'Timing campaign directory not found: %s\n' "${campaign_dir}" >&2
        exit 1
    fi

    local previous_progress=''
    while true; do
        local state
        state=$(cat "${campaign_dir}/state" 2>/dev/null || echo starting)

        local summaries=("${campaign_dir}"/run-*/summary.json)
        local completed_runs=0
        if [[ -e ${summaries[0]} ]]; then
            completed_runs=${#summaries[@]}
        fi

        local progress="${state}:${completed_runs}"
        if [[ "${progress}" != "${previous_progress}" ]]; then
            printf '[%s] Campaign %s; %s completed run(s)\n' \
                "$(timestamp)" "${state}" "${completed_runs}"
            previous_progress=${progress}
        fi

        case ${state} in
            completed | stopped)
                return
                ;;
            failed)
                return 1
                ;;
        esac

        sleep 30
    done
}

case ${1:-} in
    '')
        stop_live_instance
        start_run 2h
        ;;
    start)
        shift
        start_run "$@"
        ;;
    restart)
        shift
        restart_run "$@"
        ;;
    run)
        (($# >= 3 && $# <= 7)) || { usage >&2; exit 2; }
        if (($# >= 4)) && [[ -n $4 ]] && [[ ! $4 =~ ^[1-8]$ ]]; then
            usage >&2
            exit 2
        fi
        if (($# >= 6)) && [[ -n $6 ]] && [[ ! $6 =~ ^[1-9][0-9]*$ ]]; then
            usage >&2
            exit 2
        fi
        if (($# >= 7)) && [[ -n $7 ]] && [[ ! $7 =~ ^[1-9][0-9]*$ ]]; then
            usage >&2
            exit 2
        fi
        if { [[ -n ${6:-} ]] && [[ -z ${7:-} ]]; } || { [[ -z ${6:-} ]] && [[ -n ${7:-} ]]; }; then
            usage >&2
            exit 2
        fi
        run_soak "$(duration_seconds "$2")" "$(resolve_run_dir "$3")" "${4:-}" "${5:-}" false "${6:-}" "${7:-}"
        ;;
    observe)
        (($# == 3)) || { usage >&2; exit 2; }
        run_soak "$(duration_seconds "$2")" "$(resolve_run_dir "$3")" '' '' true
        ;;
    status)
        show_status "${2:-}"
        ;;
    follow)
        follow_run "${2:-}"
        ;;
    snapshot)
        show_snapshot "${2:-}" "${3:-}"
        ;;
    live-snapshot)
        show_live_snapshot "${2:-}"
        ;;
    set-decklink-output-buffer)
        (($# == 2)) || { usage >&2; exit 2; }
        set_decklink_output_buffer "$2"
        ;;
    stop)
        stop_run "${2:-}"
        ;;
    report)
        show_report "${2:-}"
        ;;
    brief-report)
        show_brief_report "${2:-}"
        ;;
    campaign)
        shift
        start_campaign "$@"
        ;;
    campaign-run)
        (($# == 4)) || { usage >&2; exit 2; }
        run_campaign "$2" "$3" "$(resolve_campaign_dir "$4")"
        ;;
    campaign-status)
        show_status "$(resolve_campaign_dir "${2:-}")"
        ;;
    campaign-follow)
        follow_campaign "${2:-}"
        ;;
    campaign-stop)
        stop_run "$(resolve_campaign_dir "${2:-}")"
        ;;
    campaign-report)
        show_report "$(resolve_campaign_dir "${2:-}")"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
