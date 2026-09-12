#!/usr/bin/env python3
"""Compare production Vulkan and CUDA transfers in separate, alternating processes."""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path, default=pathlib.Path('build/miximus_transfer_benchmark'))
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--iterations', type=int, default=500)
    parser.add_argument('--repeat', type=int, default=3, help='Runs per backend; order reverses each pair')
    parser.add_argument('--device', default='')
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000 or not 1 <= args.repeat <= 20:
        parser.error('iterations must be 1..100000 and repeat 1..20')

    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)

    env = os.environ.copy()
    env.pop('MIXIMUS_GPU_TRANSFER_BACKEND', None)
    env.pop('MIXIMUS_VULKAN_VALIDATION', None)

    metadata = {
        'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'worktree_status': subprocess.check_output(['git', 'status', '--short'], text=True),
        'iterations': args.iterations,
        'repeat': args.repeat,
        'runs': [],
    }

    manifest = args.output / 'manifest.json'
    reports = {'vulkan': [], 'cuda': []}

    selected_uuid = args.device

    for repetition in range(args.repeat):
        order = ['vulkan', 'cuda'] if repetition % 2 == 0 else ['cuda', 'vulkan']
        for backend in order:
            name = f'{repetition + 1}-{backend}'
            result_file = args.output / f'{name}.json'
            command = [
                str(binary),
                '--iterations', str(args.iterations),
                '--output', str(result_file),
            ]
            if selected_uuid:
                command += ['--device', selected_uuid]
            if backend == 'vulkan':
                command += ['--disable-cuda']

            print(f'Running {name}', flush=True)
            with (args.output / f'{name}.log').open('w') as log:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1800)

            metadata['runs'].append({'backend': backend, 'command': command, 'exit_code': result.returncode})
            manifest.write_text(json.dumps(metadata, indent=2) + '\n')
            if result.returncode:
                raise SystemExit(f'{name} failed; see {args.output / (name + ".log")}')

            report = json.loads(result_file.read_text())
            selected_uuid = selected_uuid or report['selected_uuid']
            expected = 'cuda-vulkan-direct' if backend == 'cuda' else 'vulkan-staging'
            valid_rows = all(
                row['backend'] == expected and row['pixels_verified']
                for row in report['results']
            )
            if (
                report['selected_uuid'] != selected_uuid
                or report['validation']
                or report['validation_errors']
                or len(report['results']) != 12
                or not valid_rows
            ):
                raise SystemExit(f'{name}: mismatched device/backend, validation enabled, or invalid pixel results')

            reports[backend].append(report)

    summary = []
    lines = ['| Size | Format | Direction | Vulkan mean ms | CUDA mean ms | CUDA/Vulkan time |',
             '| --- | --- | --- | ---: | ---: | ---: |']

    for index, first in enumerate(reports['vulkan'][0]['results']):
        case_keys = ['width', 'height', 'format', 'direction', 'bytes']
        entry = {key: first[key] for key in case_keys}
        for backend, runs in reports.items():
            rows = [run['results'][index] for run in runs]
            if any(any(row[key] != entry[key] for key in case_keys) for row in rows):
                raise SystemExit('mismatched benchmark cases')
            entry[backend] = {key: statistics.median(row[key] for row in rows)
                              for key in ['mean_us', 'p50_us', 'p95_us', 'p99_us', 'effective_GB_per_second']}

        ratio = entry['cuda']['mean_us'] / entry['vulkan']['mean_us']
        entry['cuda_over_vulkan_time'] = ratio
        summary.append(entry)
        lines.append(f"| {entry['width']}×{entry['height']} | {entry['format']} | {entry['direction']} | "
                     f"{entry['vulkan']['mean_us'] / 1000:.3f} | {entry['cuda']['mean_us'] / 1000:.3f} | {ratio:.2f}× |")

    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    text = '\n'.join(lines) + '\n'
    (args.output / 'summary.md').write_text(text)
    print(text)


if __name__ == '__main__':
    main()
