#!/usr/bin/env bash

set -euo pipefail

readonly script_path=$(realpath "$0")
readonly project_dir=$(dirname "$(dirname "${script_path}")")
readonly build_dir=${BUILD_DIR:-${project_dir}/build}
readonly source_settings=${SETTINGS:-${build_dir}/settings.json}
readonly api_url=${MIXIMUS_API_URL:-http://127.0.0.1:7351/api/v1}
readonly transition_timeout=${TRANSITION_TIMEOUT_SECONDS:-10}
readonly mode_dwell=${KEYER_MODE_DWELL_SECONDS:-4}
readonly run_dir="${build_dir}/integration-tests/decklink-keyer-$(date +%Y%m%d-%H%M%S)"
readonly test_settings="${run_dir}/settings.json"
readonly log_file="${run_dir}/test.log"
readonly samples_file="${run_dir}/status-samples.jsonl"

app_pid=''

timestamp()
{
    local value
    value=$(date '+%Y-%m-%d %H:%M:%S.%N')
    printf '%s\n' "${value:0:23}"
}

test_log()
{
    local level=$1
    shift
    printf '[%s] [test] [%s] %s\n' "$(timestamp)" "${level}" "$*"
}

app_running()
{
    [[ -n ${app_pid} ]] && kill -0 "${app_pid}" 2>/dev/null
}

cleanup()
{
    local status=$?
    trap - EXIT INT TERM
    if app_running; then
        test_log info "Stopping Miximus process ${app_pid}"
        kill -INT "${app_pid}" 2>/dev/null || true
    fi
    if [[ -n ${app_pid} ]]; then
        wait "${app_pid}" || true
    fi
    if ((status != 0)); then
        test_log error "DeckLink keyer-mode test failed. Artifacts: ${run_dir}"
    fi
    exit "${status}"
}

wait_for_api()
{
    for _ in $(seq 1 $((transition_timeout * 10))); do
        if curl -fsS "${api_url}/config" >/dev/null 2>&1; then
            return
        fi
        if ! app_running; then
            test_log error 'Miximus exited before its API became ready'
            return 1
        fi
        sleep 0.1
    done
    test_log error 'Timed out waiting for the Miximus API'
    return 1
}

post_keyer_mode()
{
    local target_node_id=$1
    local keyer_mode=$2
    local payload
    payload=$(jq -nc --arg id "${target_node_id}" --arg mode "${keyer_mode}" \
        '{action: "command", topic: "update_node", id: $id, options: {keyer_mode: $mode}}')
    curl -fsS -X POST -H 'Content-Type: application/json' --data-binary "${payload}" "${api_url}/control" >/dev/null
}

sample_mode()
{
    local target_node_id=$1
    local keyer_mode=$2
    local status
    status=$(curl -fsS "${api_url}/status")
    jq -nc \
        --arg sampled_at "$(timestamp)" \
        --arg id "${target_node_id}" \
        --arg mode "${keyer_mode}" \
        --argjson status "${status}" \
        '{sampled_at: $sampled_at, node_id: $id, requested_test_mode: $mode, status: ($status[$id] // {})}' \
        | tee -a "${samples_file}"
}

wait_for_active_mode()
{
    local target_node_id=$1
    local keyer_mode=$2
    local status

    for _ in $(seq 1 $((transition_timeout * 10))); do
        status=$(curl -fsS "${api_url}/status")
        if jq -e --arg id "${target_node_id}" --arg mode "${keyer_mode}" \
            '(.[$id].connected == true) and
             (.[$id].requested_keyer_mode == $mode) and
             (.[$id].active_keyer_mode == $mode)' <<<"${status}" >/dev/null; then
            return
        fi
        if ! app_running; then
            test_log error "Miximus exited while activating ${keyer_mode} keying"
            return 1
        fi
        sleep 0.1
    done

    test_log error "Timed out waiting for ${keyer_mode} keying to become active"
    jq -c --arg id "${target_node_id}" '.[$id] // {}' <<<"${status}" >&2
    return 1
}

for command in curl jq realpath seq tee; do
    command -v "${command}" >/dev/null || { test_log error "Required command not found: ${command}"; exit 2; }
done
[[ -x "${build_dir}/miximus" ]] || { test_log error "Miximus executable not found: ${build_dir}/miximus"; exit 2; }
[[ -f "${source_settings}" ]] || { test_log error "Settings file not found: ${source_settings}"; exit 2; }

if curl -fsS "${api_url}/config" >/dev/null 2>&1; then
    test_log error 'A Miximus instance is already running'
    exit 1
fi

mkdir -p "${run_dir}"
cp "${source_settings}" "${test_settings}"

readonly selected_node_id=${1:-$(jq -er '[.nodes[] | select(.type == "decklink_output") | .id][0]' "${test_settings}")}
if ! jq -e --arg id "${selected_node_id}" '.nodes[] | select(.id == $id and .type == "decklink_output")' \
    "${test_settings}" >/dev/null; then
    test_log error "DeckLink output node not found: ${selected_node_id}"
    exit 2
fi

jq --arg id "${selected_node_id}" \
    '(.nodes[] | select(.id == $id) | .options.keyer_mode) = "disabled"' \
    "${test_settings}" >"${test_settings}.tmp"
mv "${test_settings}.tmp" "${test_settings}"

exec > >(tee "${log_file}") 2>&1
trap cleanup EXIT INT TERM

readonly stop_after=$((3 * mode_dwell + transition_timeout + 30))
test_log info "Starting Miximus with private settings for DeckLink output ${selected_node_id}"
"${build_dir}/miximus" --settings "${test_settings}" --stop-after "${stop_after}" &
app_pid=$!
test_log info "Miximus process ID: ${app_pid}"
wait_for_api

for keyer_mode in disabled internal external; do
    test_log info "Selecting keyer mode ${keyer_mode}"
    post_keyer_mode "${selected_node_id}" "${keyer_mode}"
    wait_for_active_mode "${selected_node_id}" "${keyer_mode}"
    sleep "${mode_dwell}"
    if ! app_running; then
        test_log error "Miximus exited while testing ${keyer_mode}"
        exit 1
    fi
    sample_mode "${selected_node_id}" "${keyer_mode}"
done

test_log info "DeckLink keyer-mode sampling complete. Artifacts: ${run_dir}"
