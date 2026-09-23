#!/usr/bin/env python3
"""Manual CEF load/cadence campaign. Uses private settings and retains raw status samples."""

import argparse
import json
import pathlib
import signal
import shutil
import subprocess
import time
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = "http://127.0.0.1:7351/api/v1/config"


def snapshot():
    with urllib.request.urlopen(API, timeout=2) as response:
        result = json.load(response)
    return result.get("config", result)["status"]


def run_case(work, count, size, args):
    case = work / f"{count}-{size[0]}"
    case.mkdir()
    nodes = [
        {"id": "grid", "type": "infinite_multiviewer", "options": {}},
        {"id": "tex", "type": "framebuffer_to_texture", "options": {}},
        {"id": "screen", "type": "screen_output", "schema_version": 3,
         "options": {"size": [960, 540], "position": [0, 0]}},
    ]
    connections = [
        {"from_node": "grid", "from_interface": "fb_out", "to_node": "tex", "to_interface": "fb"},
        {"from_node": "tex", "from_interface": "tex", "to_node": "screen", "to_interface": "tex"},
    ]
    for index in range(count):
        page = (
            "<style>@keyframes move{to{transform:translate(900px,500px)}}</style>"
            "<body style='background:transparent'>"
            f"<div style='width:400px;height:400px;background:hsla({index * 45},90%,50%,.5);"
            "animation:move 1s infinite alternate'></div>"
        )
        nodes.append({"id": f"cef-{index}", "type": "cef_browser", "options": {
            "size": size, "url": "data:text/html," + urllib.parse.quote(page),
        }})
        connections.append({"from_node": f"cef-{index}", "from_interface": "tex",
                            "to_node": "grid", "to_interface": "tex"})
    settings = case / "settings.json"
    settings.write_text(json.dumps({"schema_version": 1, "nodes": nodes, "connections": connections}))
    samples = []
    system_samples = []
    next_system_sample = 0.0
    with (case / "app.log").open("w") as log:
        process = subprocess.Popen([
            str(ROOT / "build/miximus"), "--settings", str(settings),
            "--stop-after", str(args.duration), "--test-render-delay-ms", str(args.delay_ms),
            "--test-render-delay-every", "120",
        ], stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        start = time.monotonic()
        try:
            while process.poll() is None and time.monotonic() - start < args.duration + 15:
                try:
                    samples.append({"elapsed": time.monotonic() - start, "status": snapshot()})
                except OSError:
                    pass
                if args.gpu_telemetry and time.monotonic() >= next_system_sample:
                    next_system_sample = time.monotonic() + 5
                    query = subprocess.run([
                        "nvidia-smi", "--query-gpu=timestamp,name,pstate,temperature.gpu,utilization.gpu,"
                        "memory.used,memory.total,clocks.gr,clocks.mem,power.draw", "--format=csv",
                    ], capture_output=True, text=True, timeout=3, check=False)
                    system_samples.append({"elapsed": time.monotonic() - start,
                                           "gpu": query.stdout, "error": query.stderr})
                time.sleep(0.5)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            try:
                result = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            (case / "samples.json").write_text(json.dumps(samples, indent=2))
            (case / "system.json").write_text(json.dumps(system_samples, indent=2))
    stable = [sample for sample in samples if sample["elapsed"] >= args.warmup]
    if result or len(stable) < 5:
        raise RuntimeError("Failed case " + str(case))
    first, last = stable[0], stable[-1]

    def delta(node, key):
        return last["status"][node].get(key, 0) - first["status"][node].get(key, 0)

    seconds = last["elapsed"] - first["elapsed"]
    report = {
        "sources": count, "size": size, "seconds": seconds,
        "deadline_misses": delta("$app", "deadline_misses_total"),
        "skipped": delta("$app", "skipped_frames_total"),
        "injections": delta("$app", "test_render_delay_injections"), "browsers": [],
    }
    for index in range(count):
        node = f"cef-{index}"
        current = last["status"][node]
        report["browsers"].append({
            "state": current.get("cef_state"), "error": current.get("cef_error"),
            # Status is rate-limited; this is approximate, not callback timing.
            "fps": delta(node, "cef_copies") / seconds,
            "capacity_drops": delta(node, "cef_capacity_drops"),
            "queue_overflows": delta(node, "source_queue_overflow_drops"),
            "repeats": delta(node, "source_queue_repeated"),
        })
    text = (case / "app.log").read_text()
    report["validation_errors"] = "Validation Error" in text or "VUID-" in text
    report["shutdown_complete"] = "Application shutdown complete" in text
    (case / "report.json").write_text(json.dumps(report, indent=2))
    if report["validation_errors"] or not report["shutdown_complete"] or any(
        browser["state"] != "ready" or browser["error"] for browser in report["browsers"]
    ):
        raise RuntimeError("Inspect failed case " + str(case))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=int, default=95)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--delay-ms", type=int, default=20)
    parser.add_argument("--four-uhd-only", action="store_true")
    parser.add_argument("--gpu-telemetry", action="store_true", help="Sample NVIDIA clocks, load and memory every five seconds")
    args = parser.parse_args()
    if args.gpu_telemetry and not shutil.which("nvidia-smi"):
        parser.error("GPU telemetry requires nvidia-smi")
    if args.warmup < 0 or args.duration < args.warmup + 5 or args.delay_ms < 0:
        parser.error("Require nonnegative warmup/delay and at least five measured seconds")
    try:
        snapshot()
    except OSError:
        pass
    else:
        raise SystemExit("An app is already serving the API; refusing to replace it")
    work = ROOT / "build/integration-tests" / time.strftime("cef-load-%Y%m%d-%H%M%S")
    work.mkdir(parents=True, exist_ok=False)
    (work / "arguments.json").write_text(json.dumps(vars(args), indent=2))
    cases = [(4, [3840, 2160])] if args.four_uhd_only else [
        (0, [1920, 1080]), (1, [1920, 1080]), (4, [1920, 1080]),
        (8, [1920, 1080]), (1, [3840, 2160]), (4, [3840, 2160]),
    ]
    print("Artifacts:", work, flush=True)
    summary = []
    for count, size in cases:
        report = run_case(work, count, size, args)
        summary.append(report)
        (work / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
