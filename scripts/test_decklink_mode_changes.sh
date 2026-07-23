#!/usr/bin/env bash

set -euo pipefail

test_log()
{
    local level=$1
    local timestamp
    shift
    timestamp=$(date '+%Y-%m-%d %H:%M:%S.%N')
    printf '[%s] [test] [%s] %s\n' "${timestamp:0:23}" "${level}" "$*"
}

if (($# < 1)); then
    test_log error "Usage: $0 DECKLINK_OUTPUT_NODE_ID [DISPLAY_MODE ...]" >&2
    exit 2
fi

readonly node_id=$1
shift

readonly build_dir=${BUILD_DIR:-build}
readonly source_settings=${SETTINGS:-${build_dir}/settings.json}
readonly api_url=${MIXIMUS_API_URL:-http://127.0.0.1:7351/api/v1}
readonly transition_timeout=${TRANSITION_TIMEOUT_SECONDS:-10}
readonly startup_settle=${STARTUP_SETTLE_SECONDS:-2}
readonly mode_dwell=${MODE_DWELL_SECONDS:-2}

if (($# == 0)); then
    readonly -a requested_modes=(720p60)
else
    readonly -a requested_modes=("$@")
fi

if [[ ! -x "${build_dir}/miximus" ]]; then
    test_log error "Miximus executable not found at ${build_dir}/miximus" >&2
    exit 2
fi

if [[ ! -f "${source_settings}" ]]; then
    test_log error "Settings file not found at ${source_settings}" >&2
    exit 2
fi

for command in curl jq tee; do
    if ! command -v "${command}" >/dev/null; then
        test_log error "Required command not found: ${command}" >&2
        exit 2
    fi
done

readonly original_mode=$(jq -er --arg id "${node_id}" \
    '.nodes[] | select(.id == $id) | .options.display_mode' "${source_settings}")

readonly test_dir="${build_dir}/integration-tests"
mkdir -p "${test_dir}"

readonly run_id=$(date +%Y%m%d-%H%M%S)
readonly test_settings="${test_dir}/decklink-mode-change-${run_id}.json"
readonly log_file="${test_dir}/decklink-mode-change-${run_id}.log"
cp "${source_settings}" "${test_settings}"
exec > >(tee "${log_file}") 2>&1

app_pid=''
api_ready=false
restored=false

post_mode()
{
    local mode=$1
    local payload
    payload=$(jq -nc \
        --arg id "${node_id}" \
        --arg mode "${mode}" \
        '{action: "command", topic: "update_node", id: $id, options: {display_mode: $mode}}')

    curl -fsS \
        -X POST \
        -H 'Content-Type: application/json' \
        --data "${payload}" \
        "${api_url}/control"
}

log_count()
{
    local pattern=$1
    grep -Fc "${pattern}" "${log_file}" 2>/dev/null || true
}

wait_for_api()
{
    local elapsed=0
    while ((elapsed < transition_timeout * 10)); do
        if curl -fsS "${api_url}/config" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "${app_pid}" 2>/dev/null; then
            test_log error "Miximus exited before its API became ready"
            return 1
        fi
        sleep 0.1
        ((elapsed += 1))
    done

    test_log error "Timed out waiting for the Miximus API"
    return 1
}

wait_for_log_increment()
{
    local pattern=$1
    local previous_count=$2
    local elapsed=0

    while ((elapsed < transition_timeout * 10)); do
        if (( $(log_count "${pattern}") > previous_count )); then
            return 0
        fi
        if ! kill -0 "${app_pid}" 2>/dev/null; then
            test_log error "Miximus exited while waiting for: ${pattern}"
            return 1
        fi
        sleep 0.1
        ((elapsed += 1))
    done

    test_log error "Timed out waiting for: ${pattern}"
    return 1
}

change_mode()
{
    local mode=$1
    local output_count
    local input_count
    output_count=$(log_count 'DeckLink output playback running')
    input_count=$(log_count 'DeckLink input capture running')

    test_log info "Changing DeckLink output ${node_id} to ${mode}"
    post_mode "${mode}"
    wait_for_log_increment 'DeckLink output playback running' "${output_count}"
    wait_for_log_increment 'DeckLink input capture running' "${input_count}"
    test_log info "Observing ${mode} for ${mode_dwell} second(s)"
    sleep "${mode_dwell}"
}

stop_app()
{
    if [[ -z "${app_pid}" ]] || ! kill -0 "${app_pid}" 2>/dev/null; then
        return
    fi

    local pid=${app_pid}
    app_pid=''
    kill -INT "${pid}"
    wait "${pid}"
}

cleanup()
{
    local status=$?
    trap - EXIT INT TERM

    if [[ -n "${app_pid}" ]] && kill -0 "${app_pid}" 2>/dev/null; then
        if [[ "${api_ready}" == true && "${restored}" == false ]]; then
            test_log info "Restoring DeckLink output to ${original_mode}"
            post_mode "${original_mode}" || true
            sleep 1
        fi
        stop_app || true
    fi

    rm -f "${test_settings}"

    if ((status != 0)); then
        test_log error "DeckLink mode-change test failed. Log: ${log_file}"
    fi

    exit "${status}"
}

trap cleanup EXIT INT TERM

test_log info "Starting Miximus with temporary settings ${test_settings}"
"${build_dir}/miximus" --settings "${test_settings}" &
app_pid=$!

wait_for_api
api_ready=true
wait_for_log_increment 'DeckLink output playback running' 0
wait_for_log_increment 'DeckLink input capture running' 0
sleep "${startup_settle}"

current_mode=${original_mode}
for mode in "${requested_modes[@]}"; do
    if [[ "${mode}" == "${current_mode}" ]]; then
        test_log info "Skipping unchanged display mode ${mode}"
        continue
    fi
    change_mode "${mode}"
    current_mode=${mode}
done

if [[ "${current_mode}" != "${original_mode}" ]]; then
    change_mode "${original_mode}"
fi
restored=true

if grep -Fq 'VideoInputFrameArrived dropped frame: no upload slot' "${log_file}"; then
    test_log error "Unexpected DeckLink upload-slot drop was logged"
    exit 1
fi

if grep -Fq 'Timed input upload failed' "${log_file}"; then
    test_log error "A timed input upload failed"
    exit 1
fi

stop_app
test_log info "DeckLink mode-change test passed. Log: ${log_file}"
