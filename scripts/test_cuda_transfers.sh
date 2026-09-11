#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(realpath -m "${1:-${project_dir}/build}")
repeats=${2:-3}
binary="${build_dir}/src/gpu/gpu_transfer_vulkan_test"
if [[ ! ${repeats} =~ ^[1-9][0-9]*$ ]]; then
    echo 'Repeat count must be a positive integer.' >&2
    exit 2
fi
if [[ ! -x ${binary} ]]; then
    echo "Missing ${binary}; configure with BUILD_TESTING=ON and MIXIMUS_ENABLE_CUDA=ON, then build." >&2
    exit 2
fi
run_dir="${build_dir}/integration-tests/cuda-transfers-$(date -u +%Y%m%dT%H%M%S)-$$"
mkdir -p "${run_dir}"
echo 'Requiring CUDA: fallback to Vulkan staging is forbidden.'
"${binary}" --use-cuda --log-debug --gtest_repeat="${repeats}" 2>&1 | tee "${run_dir}/tests.log"
for direction in upload readback; do
    if ! rg -q "Transfer completed: backend=cuda-vulkan-buffer direction=${direction}" "${run_dir}/tests.log"; then
        echo "No completed CUDA ${direction} found; verification failed." >&2
        exit 1
    fi
done
if rg -q 'backend=vulkan-staging' "${run_dir}/tests.log"; then
    echo 'Unexpected Vulkan fallback; verification failed.' >&2
    exit 1
fi
echo "PASS: actual CUDA uploads and readbacks completed, pixel/lease tests passed, and no fallback occurred. Log: ${run_dir}/tests.log"
