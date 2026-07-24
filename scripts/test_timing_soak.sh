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
                          Start a detached soak (default: 8h)
  $0 run DURATION DIR [FRAMES]
                          Run the coordinator in the foreground
  $0 status [DIR]         Show the current run state and recent events
  $0 stop [DIR]           Gracefully stop a running soak
  $0 report [DIR]         Print the completed run summary

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

require_commands()
{
    local command
    for command in curl jq ps realpath tee; do
        if ! command -v "${command}" >/dev/null; then
            printf 'Required command not found: %s\n' "${command}" >&2
            exit 2
        fi
    done
}

start_run()
{
    local duration=${1:-8h}
    local output_buffer_frames=''
    if (($# > 1)); then
        if (($# != 3)) || [[ $2 != --decklink-output-buffer ]] || [[ ! $3 =~ ^[1-8]$ ]]; then
            usage >&2
            exit 2
        fi
        output_buffer_frames=$3
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
            "${environment[@]}" "${script_path}" run "${seconds}" "${run_dir}" "${output_buffer_frames}"
        printf 'Started timing soak %s for %ss (user service %s).\n' "${run_dir}" "${seconds}" "${unit}"
    else
        nohup "${script_path}" run "${seconds}" "${run_dir}" "${output_buffer_frames}" \
            >"${run_dir}/bootstrap.log" 2>&1 &
        local runner_pid=$!
        printf '%s\n' "${runner_pid}" >"${run_dir}/runner.pid"
        printf 'Started timing soak %s for %ss (runner PID %s).\n' "${run_dir}" "${seconds}" "${runner_pid}"
    fi
}

run_soak()
{
    local seconds=$1
    local run_dir=$2
    local output_buffer_frames=${3:-}
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
        if app_running; then
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

    cp "${source_settings}" "${settings_file}"
    if [[ -n "${output_buffer_frames}" ]]; then
        local rewritten_settings="${settings_file}.tmp"
        jq --argjson frames "${output_buffer_frames}" '
            (.nodes[] | select(.id == "$app").options.decklink_output_buffer_frames) = $frames' \
            "${settings_file}" >"${rewritten_settings}"
        mv "${rewritten_settings}" "${settings_file}"
    fi
    printf 'starting\n' >"${run_dir}/state"
    printf '%s\n' "$$" >"${run_dir}/runner.pid"
    : >"${events_file}"
    : >"${samples_file}"
    : >"${system_file}"
    mkfifo "${soak_fifo}"

    while IFS= read -r line; do
        printf '[app] %s\n' "${line}" | tee -a "${events_file}"
    done <"${soak_fifo}" &
    soak_logger_pid=$!

    log_event info "Starting Miximus for ${seconds}s with ${settings_file}"
    "${build_dir}/miximus" --settings "${settings_file}" --stop-after "$((seconds + 60))" >"${soak_fifo}" 2>&1 &
    soak_app_pid=$!
    printf '%s\n' "${soak_app_pid}" >"${run_dir}/app.pid"

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
        program_queue_overflow_drops
        program_timing_drops
        program_starvation_repeats
        output_refill_shortfalls
        buffered_zero_samples
        render_target_drops
        upload_slot_drops
        source_queue_overflow_drops
        source_queue_transfer_failures
        program_frames_missing
        program_starvation_repeat_streak_max
        buffered_below_target_samples
        output_intervals_skipped
        receiver_video_drops
        invalid_frames
    )
    local anomaly_keys_json
    anomaly_keys_json=$(printf '%s\n' "${anomaly_keys[@]}" | jq -R . | jq -sc .)

    local start_epoch_ns
    start_epoch_ns=$(date +%s%N)
    local next_gpu_sample=0
    while [[ "${stopping}" != true ]] && app_running; do
        local now_epoch_ns elapsed wall_time
        now_epoch_ns=$(date +%s%N)
        elapsed=$(((now_epoch_ns - start_epoch_ns) / 1000000000))
        wall_time=$(date --iso-8601=ns)
        if ((elapsed >= seconds)); then
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
            sleep 1
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
        sleep 1
    done

    if app_running; then
        kill -INT "${soak_app_pid}"
    fi
    wait "${soak_app_pid}" || soak_exit_code=$?
    soak_app_pid=''
    wait "${soak_logger_pid}" 2>/dev/null || true
    soak_logger_pid=''
    rm -f "${soak_fifo}"

    local end_epoch_ns
    end_epoch_ns=$(date +%s%N)
    jq -n --slurpfile samples "${samples_file}" --slurpfile system "${system_file}" \
        --argjson anomaly_keys "${anomaly_keys_json}" --argjson requested_seconds "${seconds}" \
        --arg output_buffer_frames "${output_buffer_frames}" \
        --argjson elapsed_seconds "$(((end_epoch_ns - start_epoch_ns) / 1000000000))" \
        --argjson app_exit_code "${soak_exit_code}" '
        {
            requested_seconds: $requested_seconds,
            elapsed_seconds: $elapsed_seconds,
            app_exit_code: $app_exit_code,
            decklink_output_buffer_frames_override:
                (if $output_buffer_frames == "" then null else ($output_buffer_frames | tonumber) end),
            system: {
                cpu_percent_max: ([$system[] | .cpu_percent // empty] | max // null),
                rss_kib_max: ([$system[] | .rss_kib // empty] | max // null),
                threads_max: ([$system[] | .threads // empty] | max // null),
                gpu_memory_mib_max: ([$system[] | .gpu_memory_mib // empty] | max // null)
            },
            nodes: ($samples
                | group_by(.node_id)
                | map(. as $node | {
                    id: $node[0].node_id,
                    type: $node[0].node_type,
                    first_status: $node[0].status,
                    last_status: $node[-1].status,
                    anomaly_counter_deltas: (reduce $anomaly_keys[] as $key ({};
                        [$node[].status[$key] | select(type == "number")] as $values
                        | if ($values | length) > 0
                          then .[$key] = $values[-1] - $values[0]
                          else . end))
                }))
        }' >"${run_dir}/summary.json"

    if ((soak_exit_code == 0)); then
        printf 'completed\n' >"${run_dir}/state"
        log_event info "Timing soak completed. Summary: ${run_dir}/summary.json"
    else
        printf 'failed\n' >"${run_dir}/state"
        log_event error "Miximus exited with status ${soak_exit_code}"
    fi
    trap - EXIT
    return "${soak_exit_code}"
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

case ${1:-} in
    start)
        shift
        start_run "$@"
        ;;
    run)
        (($# == 3 || $# == 4)) || { usage >&2; exit 2; }
        if (($# == 4)) && [[ -n $4 ]] && [[ ! $4 =~ ^[1-8]$ ]]; then
            usage >&2
            exit 2
        fi
        run_soak "$(duration_seconds "$2")" "$(resolve_run_dir "$3")" "${4:-}"
        ;;
    status)
        show_status "${2:-}"
        ;;
    stop)
        stop_run "${2:-}"
        ;;
    report)
        show_report "${2:-}"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
