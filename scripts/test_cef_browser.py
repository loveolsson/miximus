#!/usr/bin/env python3
"""Manual Linux/NVIDIA CEF node lifecycle test using isolated settings."""

import json
import pathlib
import signal
import subprocess
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = "http://127.0.0.1:7351/api/v1"


def config():
    with urllib.request.urlopen(API + "/config", timeout=2) as response:
        return json.load(response)


def update(options):
    data = json.dumps({
        "action": "command", "topic": "update_node", "id": "browser", "options": options,
    }).encode()
    request = urllib.request.Request(
        API + "/control", data=data, headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=2) as response:
        print("update", options, response.status, flush=True)


def await_status(predicate):
    deadline = time.monotonic() + 15
    snapshot = {}
    while time.monotonic() < deadline:
        try:
            snapshot = config()
            status = snapshot.get("config", snapshot).get("status", {}).get("browser", {})
            if predicate(status):
                print("status", status, flush=True)
                return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError("Status target not reached: " + str(snapshot))


def main():
    try:
        config()
    except OSError:
        pass
    else:
        raise SystemExit("An app is already serving the API; refusing to replace it")

    work = ROOT / "build/integration-tests" / time.strftime("cef-node-%Y%m%d-%H%M%S")
    work.mkdir(parents=True, exist_ok=False)
    settings = work / "settings.json"
    settings.write_text(json.dumps({
        "schema_version": 1,
        "nodes": [
            {"id": "browser", "type": "cef_browser", "schema_version": 1, "options": {
                "size": [640, 360],
                "url": "data:text/html,<style>@keyframes move{to{transform:translateX(300px)}}</style>"
                       "<body style='background:transparent'><div style='width:100px;height:100px;"
                       "background:rgba(255,0,0,.5);animation:move 1s infinite alternate'></div>",
            }},
            {"id": "screen", "type": "screen_output", "schema_version": 3, "options": {
                "size": [640, 360], "position": [0, 0], "fullscreen": False, "enabled": True,
            }},
        ],
        "connections": [{
            "from_node": "browser", "from_interface": "tex", "to_node": "screen", "to_interface": "tex",
        }],
    }))
    print("Artifacts:", work, flush=True)
    with (work / "app.log").open("w") as log:
        process = subprocess.Popen(
            [str(ROOT / "build/miximus"), "--settings", str(settings), "--stop-after", "35"],
            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            await_status(lambda s: s.get("cef_copies", 0) >= 60 and s.get("connected"))
            update({"size": [800, 450], "url": "data:text/html,<body style='background:blue'>replacement</body>"})
            await_status(lambda s: s.get("cef_copies", 0) > 0 and s.get("source_queue_repeated", 0) > 30
                         and s.get("connected"))
            update({"enabled": False})
            await_status(lambda s: s.get("cef_state") == "stopped")
            update({"enabled": True})
            await_status(lambda s: s.get("cef_copies", 0) > 0 and s.get("connected"))
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
            try:
                result = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
        if result:
            raise RuntimeError("App exited " + str(result))
    text = (work / "app.log").read_text()
    if "Validation Error" in text or "VUID-" in text:
        raise RuntimeError("Inspect Vulkan validation log")
    if "Application shutdown complete" not in text:
        raise RuntimeError("Normal shutdown did not complete")
    print("CEF node replacement, disable/enable, ordinary screen consumption and shutdown passed", flush=True)


if __name__ == "__main__":
    main()
