#!/usr/bin/env bash

set -euo pipefail

readonly script_path=$(realpath "$0")
readonly project_dir=$(dirname "$(dirname "${script_path}")")
readonly build_dir=${BUILD_DIR:-${project_dir}/build}
readonly source_settings=${SETTINGS:-${build_dir}/settings.json}
readonly api_url=${MIXIMUS_API_URL:-http://127.0.0.1:7351/api/v1}
readonly dwell_seconds=${SCREEN_TEST_DWELL_SECONDS:-8}
readonly cycle_count=${SCREEN_TEST_CYCLES:-2}
readonly run_dir="${build_dir}/integration-tests/screen-output-$(date +%Y%m%d-%H%M%S)"
readonly test_settings="${run_dir}/settings.json"
readonly log_file="${run_dir}/test.log"

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
    trap - EXIT INT TERM
    if app_running; then
        test_log info "Stopping Miximus process ${app_pid}"
        kill -INT "${app_pid}" 2>/dev/null || true
    fi
    if [[ -n ${app_pid} ]]; then
        wait "${app_pid}" || true
    fi
}

post_options()
{
    local node_id=$1
    local options=$2
    local payload
    payload=$(jq -nc --arg id "${node_id}" --argjson options "${options}" \
        '{action: "command", topic: "update_node", id: $id, options: $options}')
    curl -fsS -X POST -H 'Content-Type: application/json' --data-binary "${payload}" "${api_url}/control"
}

wait_for_api()
{
    for _ in {1..300}; do
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

dwell()
{
    local description=$1
    test_log info "Observing ${description} for ${dwell_seconds}s"
    for ((second = 0; second < dwell_seconds; ++second)); do
        sleep 1
        if ! app_running; then
            test_log error "Miximus exited while testing ${description}"
            return 1
        fi
    done
}

for command in curl jq realpath tee; do
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
exec > >(tee "${log_file}") 2>&1
trap cleanup EXIT INT TERM

readonly screen_id=$(jq -er '[.nodes[] | select(.type == "screen_output") | .id][0]' "${test_settings}")
readonly stop_after=$((cycle_count * 16 * dwell_seconds + 60))

test_log info "Starting Miximus with screen node ${screen_id}"
"${build_dir}/miximus" --settings "${test_settings}" --stop-after "${stop_after}" &
app_pid=$!
test_log info "Miximus process ID: ${app_pid}"
wait_for_api

mapfile -t monitors < <(
    for _ in {1..100}; do
        status=$(curl -fsS "${api_url}/status")
        if jq -e --arg id "${screen_id}" '.[$id].monitors | length > 0' <<<"${status}" >/dev/null; then
            jq -r --arg id "${screen_id}" '.[$id].monitors[].id' <<<"${status}"
            break
        fi
        sleep 0.1
    done
)
if ((${#monitors[@]} == 0)); then
    test_log error 'No monitors were reported by the screen output node'
    exit 1
fi

for ((cycle = 1; cycle <= cycle_count; ++cycle)); do
    test_log info "Starting screen-output change cycle ${cycle}/${cycle_count}"

    post_options "${screen_id}" '{"fullscreen":false,"position":[5100,3300]}'
    dwell 'windowed position change'

    for monitor_id in "${monitors[@]}"; do
        post_options "${screen_id}" "$(jq -nc --arg id "${monitor_id}" '{fullscreen:true, monitor_id:$id}')"
        dwell "fullscreen monitor ${monitor_id}"
    done

    post_options "${screen_id}" '{"fullscreen":false,"position":[5000,3300]}'
    dwell 'return to windowed mode'
done

test_log info 'All screen-output changes completed with Miximus still running'
