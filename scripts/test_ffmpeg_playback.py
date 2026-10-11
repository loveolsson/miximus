#!/usr/bin/env python3
"""Exercise looping playback and inactivity through the existing CEF input harness.

Uses private settings and generated YUV media; refuses to alter a running app.
Pass --filename FILE --expect-hardware to qualify a real compressed codec sample.
"""
import argparse
import json
import subprocess
import threading
import time
import urllib.request
from http.server import ThreadingHTTPServer
from cef_test_process import PROCESS_CREATION_FLAGS, stop_process
from test_cef_inputs import API, ROOT, Page, command, config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--filename")
    parser.add_argument("--expect-hardware", action="store_true")
    parser.add_argument("--use-cuda", action="store_true")
    parser.add_argument("--stall-render", action="store_true", help="Inject render stalls and require overdue frame drops")
    args = parser.parse_args()
    try:
        config()
    except OSError:
        pass
    else:
        raise RuntimeError("A Miximus instance is already serving the API")
    work = ROOT / "build/integration-tests" / time.strftime("ffmpeg-%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    filename = work / "video-å.y4m"
    with filename.open("wb") as video:
        video.write(b"YUV4MPEG2 W96 H64 F25:1 Ip A1:1 C420jpeg\n")
        for frame in range(25):
            video.write(b"FRAME\n")
            video.write(bytes([16 + frame * 8]) * (96 * 64))
            video.write(bytes([128]) * (96 * 64 // 2))
    if args.filename:
        from pathlib import Path
        filename = Path(args.filename).resolve()
    Page.inputs = 1
    server = ThreadingHTTPServer(("127.0.0.1", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    settings = work / "settings.json"
    connection = dict(from_node="video", from_interface="texture", to_node="browser", to_interface="input_0")
    settings.write_text(json.dumps(dict(schema_version=1, nodes=[
        dict(id="video", type="ffmpeg_playback_looping", options=dict(filename=str(filename))),
        dict(id="browser", type="cef_browser", options=dict(url=f"http://127.0.0.1:{server.server_port}/", size=[320,180])),
    ], connections=[connection])), encoding="utf-8")
    records = []
    print("Artifacts:", work, flush=True)
    with (work / "app.log").open("w", encoding="utf-8") as log:
        app = subprocess.Popen([str(ROOT / "build/miximus"), "--settings", str(settings), "--stop-after", "120"]
                               + (["--test-render-delay-ms", "150", "--test-render-delay-every", "60"] if args.stall_render else [])
                               + (["--use-cuda"] if args.use_cuda else []), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=PROCESS_CREATION_FLAGS)

        def status():
            if app.poll() is not None:
                raise RuntimeError(f"Application exited: {app.returncode}")
            with urllib.request.urlopen(API + "/status", timeout=2) as response:
                return json.load(response).get("video", {})

        def wait(label, predicate, expect_error=False):
            deadline = time.monotonic() + 30
            latest = {}
            while time.monotonic() < deadline:
                try:
                    latest = status()
                except OSError:
                    time.sleep(.1)
                    continue
                if latest.get("playback_error") and not expect_error:
                    raise RuntimeError(str(latest))
                if predicate(latest):
                    if latest.get("connected") and latest.get("playback_backend") != "software":
                        if latest.get("playback_transfer_path") not in ["Vulkan device-local components", "D3D11 shared GPU texture to Vulkan", "VAAPI shared GPU surface to Vulkan"]:
                            raise RuntimeError("Hardware playback lost GPU residency: " + str(latest))
                    records.append(dict(label=label, status=latest))
                    print(label, latest, flush=True)
                    return latest
                time.sleep(.1)
            raise RuntimeError(f"{label} timed out: {latest}")

        try:
            first = wait("playing", lambda s: s.get("connected") and s.get("playback_position_seconds", 0) > 1)
            if args.expect_hardware and first.get("playback_backend") == "software":
                raise RuntimeError("Hardware decode fell back: " + str(first))
            if args.stall_render:
                wait("late frames dropped", lambda s: s.get("playback_dropped_frames", 0) > 0)
            if not args.filename:
                wait("looping", lambda s: s.get("playback_loops", 0) >= 2)
            running_position = status()["playback_position_seconds"]
            command("remove_connection", connection=connection)
            wait("always playing while inactive", lambda s: s.get("playback_position_seconds", 0) > running_position + .5)
            command("add_connection", connection=connection)
            command("update_node", id="video", options=dict(playback_mode="pause_inactivity"))
            command("remove_connection", connection=connection)
            time.sleep(2)
            paused = status()["playback_position_seconds"]
            time.sleep(2)
            assert status()["playback_position_seconds"] == paused, "Paused transport advanced"
            command("add_connection", connection=connection)
            wait("resumed", lambda s: s.get("playback_position_seconds", 0) > paused + .5)
            command("update_node", id="video", options=dict(playback_mode="pause_and_rewind_inactive"))
            time.sleep(.5)
            command("remove_connection", connection=connection)
            wait("rewound", lambda s: s.get("playback_position_seconds") == 0)
            time.sleep(1)
            assert status()["playback_position_seconds"] == 0
            command("add_connection", connection=connection)
            wait("restarted", lambda s: s.get("playback_position_seconds", 0) > .5)
            command("update_node", id="$app", options=dict(ffmpeg_decoded_buffer_frames=1))
            wait("one-frame queue", lambda s: s.get("connected") and s.get("playback_queue_depth", 99) <= 1)
            for _ in range(6):
                command("update_node", id="video", options=dict(filename=""))
                command("update_node", id="video", options=dict(filename=str(filename)))
            wait("replacement", lambda s: s.get("connected"))
            command("update_node", id="video", options=dict(filename=str(work / "missing-video.mp4")))
            wait("missing-file error", lambda s: bool(s.get("playback_error")), expect_error=True)
            corrupt = work / "corrupt.mp4"
            corrupt.write_bytes(b"This is not a media container")
            command("update_node", id="video", options=dict(filename=str(corrupt)))
            wait("corrupt-file error", lambda s: "Invalid data" in s.get("playback_error", ""), expect_error=True)
            command("update_node", id="video", options=dict(filename=str(filename)))
            wait("recovered", lambda s: not s.get("playback_error") and s.get("connected")
                 and s.get("playback_position_seconds", 0) > .5, expect_error=True)
            command("remove_node", id="video")
        finally:
            result = stop_process(app, timeout=20)
            server.shutdown()
            server.server_close()
            (work / "samples.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        if result:
            raise RuntimeError(f"Application exited: {result}")
    text = (work / "app.log").read_text(encoding="utf-8")
    if "Application shutdown complete" not in text or "VUID-" in text or "Validation Error" in text:
        raise RuntimeError("Inspect validation/shutdown log")
    print("Playback, looping, inactivity, rewind, replacement, error recovery and shutdown passed", flush=True)


if __name__ == "__main__":
    main()
