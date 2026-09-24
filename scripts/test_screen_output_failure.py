#!/usr/bin/env python3
"""Exercise screen failure status and explicit reconfiguration on a real display.

Usage: python3 scripts/test_screen_output_failure.py [build/miximus]
Requires an idle local API port, Vulkan presentation, and a display session.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

API = "http://127.0.0.1:7351/api/v1"


def request(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(API + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as response:
        body = response.read()
        return json.loads(body) if body else None


def main():
    try:
        request("/config")
    except (urllib.error.URLError, TimeoutError):
        pass
    else:
        raise RuntimeError("A Miximus instance is already using the test API port")

    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/miximus").resolve()
    with tempfile.TemporaryDirectory(prefix="miximus-screen-failure-") as directory:
        settings = Path(directory) / "settings.json"
        settings.write_text(json.dumps({"nodes": [{"id": "test-screen", "type": "screen_output", "schema_version": 3, "options": {
            "enabled": True, "fullscreen": True, "monitor_id": "miximus-test-missing-monitor",
            "position": [64, 64], "size": [320, 180]
        }}], "connections": []}))
        log = Path(directory) / "app.log"
        with log.open("w") as output:
            app = subprocess.Popen([str(binary), "--settings", str(settings), "--stop-after", "30"],
                                   stdout=output, stderr=subprocess.STDOUT)
            try:
                def status():
                    assert app.poll() is None, "Application exited unexpectedly"
                    return request("/status").get("test-screen", {})

                def wait_for(predicate):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        try:
                            current = status()
                            if predicate(current):
                                return current
                        except (urllib.error.URLError, TimeoutError):
                            pass
                        time.sleep(0.1)
                    raise AssertionError(f"Timed out waiting for screen status: {status()}")

                failure = wait_for(lambda s: s.get("connected") is False and "unavailable" in s.get("screen_error", ""))
                # The graph continues evaluating while the endpoint stays failed.
                for _ in range(20):
                    time.sleep(0.1)
                    current = status()
                    assert current["connected"] is False
                    assert current["screen_error"] == failure["screen_error"]
                request("/control", {"action": "command", "topic": "update_node", "id": "test-screen",
                                     "options": {"fullscreen": False}})
                wait_for(lambda s: s.get("connected") is True and s.get("screen_error") == "" and s.get("swaps_completed", 0) > 0)
                request("/control", {"action": "command", "topic": "update_node", "id": "test-screen",
                                     "options": {"enabled": False}})
                wait_for(lambda s: s.get("connected") is False and s.get("screen_error") == "")
            except BaseException:
                print(log.read_text(), file=sys.stderr)
                raise
            finally:
                app.terminate()
                try:
                    app.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
            assert app.returncode == 0, f"Application exit: {app.returncode}"
    print("PASS: unavailable monitor reported, failure retained, explicit reconfiguration presents, disable disconnects")


if __name__ == "__main__":
    main()
